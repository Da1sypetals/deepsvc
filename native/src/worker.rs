use crate::engine::{Engine, Estimator, Event, Job, JobKind, JobStateName};
use anyhow::{Context, Result, ensure};
use log::{error, info};
use std::any::Any;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};
use yingmusic::{
    Cancelled, F0Estimator, F0Estimators, InferParams, Progress, YingMusicSvc, YingMusicSvcPaths,
    load_audio_mono, resample_mono,
};

const MODEL_SAMPLE_RATE: u32 = 44_100;
const FEATURE_SAMPLE_RATE: u32 = 16_000;
const PROGRESS_INTERVAL: Duration = Duration::from_millis(50);

// 工作线程：串行消费任务队列，模型（F0 估计器与 SVC）首次使用时加载并驻留复用
pub fn run_worker(engine: Arc<Engine>) -> ! {
    let mut f0_estimators: Option<F0Estimators> = None;
    let mut svc: Option<YingMusicSvc> = None;
    loop {
        let job = engine.pop_job_blocking();
        info!("job {} started", job.id);
        let termination = map_execution(catch_unwind(AssertUnwindSafe(|| {
            execute(&job, &engine, &mut f0_estimators, &mut svc)
        })));
        engine.finish_running();
        match termination {
            Termination::Completed(outcome) => {
                match outcome {
                    Outcome::Detect { f0 } => {
                        engine.emit(&Event::DetectResult {
                            job_id: job.id,
                            f0: Arc::new(f0),
                        });
                    }
                    Outcome::Synth {
                        audio,
                        first_vocoder,
                        f0,
                    } => {
                        engine.emit(&Event::SynthResult {
                            job_id: job.id,
                            audio: Arc::new(audio),
                            first_vocoder: first_vocoder.map(Arc::new),
                            f0: Arc::new(f0),
                        });
                    }
                }
                engine.emit(&Event::job_state(
                    job.id,
                    JobStateName::Succeeded,
                    0,
                    "",
                    -1.0,
                    None,
                ));
                info!("job {} succeeded", job.id);
            }
            Termination::Cancelled => {
                engine.emit(&Event::job_state(
                    job.id,
                    JobStateName::Cancelled,
                    0,
                    "",
                    -1.0,
                    None,
                ));
                info!("job {} cancelled", job.id);
            }
            Termination::Failed(message) => {
                engine.emit(&Event::job_state(
                    job.id,
                    JobStateName::Failed,
                    0,
                    "",
                    -1.0,
                    Some(message.clone()),
                ));
                error!("job {} failed: {message}", job.id);
            }
        }
        engine.notify_queue_positions();
    }
}

enum Termination {
    Completed(Outcome),
    Failed(String),
    Cancelled,
}

fn map_execution(result: std::thread::Result<Result<Outcome>>) -> Termination {
    match result {
        Ok(Ok(outcome)) => Termination::Completed(outcome),
        Ok(Err(err)) if err.downcast_ref::<Cancelled>().is_some() => Termination::Cancelled,
        Ok(Err(err)) => Termination::Failed(format!("{err:#}")),
        Err(payload) => Termination::Failed(format!("engine panicked: {}", panic_message(payload))),
    }
}

enum Outcome {
    Detect {
        f0: Vec<f32>,
    },
    Synth {
        audio: Vec<f32>,
        first_vocoder: Option<Vec<f32>>,
        f0: Vec<f32>,
    },
}

fn execute(
    job: &Job,
    engine: &Arc<Engine>,
    f0_estimators: &mut Option<F0Estimators>,
    svc: &mut Option<YingMusicSvc>,
) -> Result<Outcome> {
    let model_dir = engine.model_dir().context("model dir is not set")?;
    match &job.kind {
        JobKind::Detect {
            pcm,
            sample_rate,
            estimator,
        } => {
            ensure_f0_estimators(engine, job, f0_estimators, &model_dir)?;
            send_running(engine, job, "detect pitch", -1.0);
            check_cancel(job)?;
            let pcm_16k = resample_mono(pcm, *sample_rate, FEATURE_SAMPLE_RATE)?;
            let f0 = f0_estimators
                .as_mut()
                .expect("f0 estimators must be loaded")
                .estimate_f0((*estimator).into(), &pcm_16k)?;
            Ok(Outcome::Detect { f0 })
        }
        JobKind::Synth {
            src,
            sample_rate,
            reference_path,
            params,
        } => {
            ensure_svc(engine, job, svc, &model_dir)?;
            send_running(engine, job, "load audio", -1.0);
            check_cancel(job)?;
            let src_44k = resample_mono(src, *sample_rate, MODEL_SAMPLE_RATE)?;
            let reference_44k = load_audio_mono(reference_path, MODEL_SAMPLE_RATE)?;
            let infer_params = InferParams {
                f0_estimator: params.f0_estimator.into(),
                diffusion_steps: params.diffusion_steps as usize,
                pitch_shift: params.pitch_shift,
                pitch_fine_tune_cents: params.pitch_fine_tune_cents,
                cfg_rate: params.cfg_rate,
                input_gain_db: params.input_gain_db,
                keep_first_vocoder_output: params.keep_first_vocoder_output,
                collect_video_mel: false,
            };
            let mut last_progress = Instant::now() - PROGRESS_INTERVAL;
            let mut progress = |event: Progress| -> bool {
                if job.cancel.load(Ordering::Relaxed) {
                    return false;
                }
                match event {
                    Progress::Diffusion { done, total } => {
                        if last_progress.elapsed() >= PROGRESS_INTERVAL || done == total {
                            send_running(
                                engine,
                                job,
                                "diffusion",
                                done as f64 / total as f64,
                            );
                            last_progress = Instant::now();
                        }
                    }
                    Progress::Stage(name) => send_running(engine, job, name, -1.0),
                    Progress::ChunkStage { name, chunk, total } => {
                        send_running(engine, job, name, chunk as f64 / total as f64)
                    }
                }
                true
            };
            let result = svc
                .as_mut()
                .expect("svc must be loaded")
                .infer_samples(&src_44k, &reference_44k, &infer_params, Some(&mut progress))?;
            Ok(Outcome::Synth {
                audio: result.audio,
                first_vocoder: result.first_vocoder,
                f0: result.f0,
            })
        }
    }
}

fn ensure_f0_estimators(
    engine: &Arc<Engine>,
    job: &Job,
    f0_estimators: &mut Option<F0Estimators>,
    model_dir: &Path,
) -> Result<()> {
    if f0_estimators.is_some() {
        return Ok(());
    }
    send_loading_models(engine, job);
    let estimators = F0Estimators::load(
        &model_dir.join("rmvpe.safetensors"),
        &model_dir.join("fcpe.safetensors"),
    )?;
    *f0_estimators = Some(estimators);
    Ok(())
}

fn ensure_svc(
    engine: &Arc<Engine>,
    job: &Job,
    svc: &mut Option<YingMusicSvc>,
    model_dir: &Path,
) -> Result<()> {
    if svc.is_some() {
        return Ok(());
    }
    send_loading_models(engine, job);
    let paths = YingMusicSvcPaths {
        whisper: model_dir.join("whisper.safetensors"),
        rmvpe: model_dir.join("rmvpe.safetensors"),
        fcpe: model_dir.join("fcpe.safetensors"),
        campplus: model_dir.join("campplus.safetensors"),
        yingmusic: model_dir.join("yingmusic_step_000640.safetensors"),
        pupu_vocoder: model_dir.join("pupu-vocoder-large.safetensors"),
        pc_nsf_hifigan: model_dir.join("pc-nsf-hifigan.safetensors"),
    };
    *svc = Some(YingMusicSvc::new(&paths)?);
    Ok(())
}

fn send_loading_models(engine: &Arc<Engine>, job: &Job) {
    engine.emit(&Event::job_state(
        job.id,
        JobStateName::LoadingModels,
        0,
        "",
        -1.0,
        None,
    ));
}

fn send_running(engine: &Arc<Engine>, job: &Job, stage: &str, fraction: f64) {
    engine.emit(&Event::job_state(
        job.id,
        JobStateName::Running,
        0,
        stage,
        fraction,
        None,
    ));
}

fn check_cancel(job: &Job) -> Result<()> {
    ensure!(
        !job.cancel.load(Ordering::Relaxed),
        "job cancelled before start"
    );
    Ok(())
}

fn panic_message(payload: Box<dyn Any + Send>) -> String {
    if let Some(message) = payload.downcast_ref::<&str>() {
        (*message).to_string()
    } else if let Some(message) = payload.downcast_ref::<String>() {
        message.clone()
    } else {
        "unknown panic".to_string()
    }
}

impl From<Estimator> for F0Estimator {
    fn from(estimator: Estimator) -> Self {
        match estimator {
            Estimator::Rmvpe => F0Estimator::Rmvpe,
            Estimator::Fcpe => F0Estimator::Fcpe,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn panic_maps_to_failed() {
        let result = catch_unwind(|| -> Result<Outcome> { panic!("boom") });
        match map_execution(result) {
            Termination::Failed(message) => assert_eq!(message, "engine panicked: boom"),
            _ => panic!("panic must map to failed"),
        }
    }

    #[test]
    fn cancelled_maps_to_cancelled() {
        let result: std::thread::Result<Result<Outcome>> = Ok(Err(Cancelled.into()));
        assert!(matches!(map_execution(result), Termination::Cancelled));
    }

    #[test]
    fn ordinary_error_maps_to_failed() {
        let result: std::thread::Result<Result<Outcome>> =
            Ok(Err(anyhow::anyhow!("bad reference")));
        match map_execution(result) {
            Termination::Failed(message) => assert!(message.contains("bad reference")),
            _ => panic!("ordinary error must map to failed"),
        }
    }
}
