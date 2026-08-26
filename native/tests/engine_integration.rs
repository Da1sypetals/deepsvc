use deepsvc_engine::engine::{self, Estimator, Event, JobKind, JobStateName, SynthParams};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::mpsc::{Receiver, channel};
use std::time::Duration;

const REFERENCE_WAV: &str = "/Users/daisy/develop/audiokit/azi.wav";
const RECV_TIMEOUT: Duration = Duration::from_secs(600);

fn model_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../checkpoints")
}

fn sine(seconds: f32, sample_rate: u32) -> Arc<[f32]> {
    let count = (seconds * sample_rate as f32) as usize;
    let samples: Vec<f32> = (0..count)
        .map(|i| {
            (2.0 * std::f32::consts::PI * 220.0 * i as f32 / sample_rate as f32).sin() * 0.5
        })
        .collect();
    samples.into()
}

fn synth_params(steps: u32) -> SynthParams {
    SynthParams {
        f0_estimator: Estimator::Rmvpe,
        diffusion_steps: steps,
        pitch_shift: 0.0,
        pitch_fine_tune_cents: 0.0,
        cfg_rate: 0.9,
        input_gain_db: 0.0,
        keep_first_vocoder_output: true,
    }
}

#[derive(Debug)]
enum Msg {
    State {
        job_id: u64,
        state: JobStateName,
        queue_position: u32,
        stage: String,
        fraction: f64,
        error: Option<String>,
    },
    Detect {
        job_id: u64,
        f0: Vec<f32>,
    },
    Synth {
        job_id: u64,
        audio: Vec<f32>,
        first_vocoder: Option<Vec<f32>>,
        f0: Vec<f32>,
    },
}

fn recv(rx: &Receiver<Msg>) -> Msg {
    rx.recv_timeout(RECV_TIMEOUT).expect("event timed out")
}

fn wait_state(rx: &Receiver<Msg>, job_id: u64, state: JobStateName) -> Msg {
    loop {
        let msg = recv(rx);
        if let Msg::State {
            job_id: id,
            state: s,
            error,
            ..
        } = &msg
        {
            if *id == job_id && *s == JobStateName::Failed && state != JobStateName::Failed {
                panic!("job {job_id} failed while waiting for {state:?}: {error:?}");
            }
            if *id == job_id && *s == state {
                return msg;
            }
        }
    }
}

#[test]
fn engine_end_to_end() {
    let engine = engine::shared();

    assert!(model_dir().is_dir(), "model dir missing: {:?}", model_dir());
    assert!(
        Path::new(REFERENCE_WAV).is_file(),
        "reference wav missing: {REFERENCE_WAV}"
    );

    // 模型目录冲突要被拒绝
    engine.set_model_dir(model_dir()).unwrap();
    assert!(engine.set_model_dir(model_dir()).is_ok(), "same dir is idempotent");
    assert!(
        engine.set_model_dir(PathBuf::from("/elsewhere")).is_err(),
        "conflicting dir must fail"
    );

    let (tx, rx) = channel::<Msg>();
    engine.add_listener(Arc::new(move |event: &Event| {
        let msg = match event {
            Event::JobState {
                job_id,
                state,
                queue_position,
                stage,
                fraction,
                error,
            } => Msg::State {
                job_id: *job_id,
                state: *state,
                queue_position: *queue_position,
                stage: stage.clone(),
                fraction: *fraction,
                error: error.clone(),
            },
            Event::DetectResult { job_id, f0 } => Msg::Detect {
                job_id: *job_id,
                f0: f0.as_ref().clone(),
            },
            Event::SynthResult {
                job_id,
                audio,
                first_vocoder,
                f0,
            } => Msg::Synth {
                job_id: *job_id,
                audio: audio.as_ref().clone(),
                first_vocoder: first_vocoder.as_ref().map(|v| v.as_ref().clone()),
                f0: f0.as_ref().clone(),
            },
        };
        tx.send(msg).unwrap();
    }));

    // 音高检测任务
    engine
        .submit(1, JobKind::Detect {
            pcm: sine(0.5, 16_000),
            sample_rate: 16_000,
            estimator: Estimator::Rmvpe,
        })
        .unwrap();
    let mut detect_f0 = None;
    loop {
        match recv(&rx) {
            Msg::Detect { job_id: 1, f0 } => detect_f0 = Some(f0),
            msg @ Msg::State { .. } => {
                if matches!(&msg, Msg::State { job_id: 1, state: JobStateName::Succeeded, .. }) {
                    break;
                }
                if matches!(&msg, Msg::State { job_id: 1, state: JobStateName::Failed, .. }) {
                    panic!("detect failed: {msg:?}");
                }
            }
            _ => {}
        }
    }
    let f0 = detect_f0.expect("detect result must arrive");
    assert!(
        (40..=60).contains(&f0.len()),
        "0.5s audio must give about 50 f0 frames, got {}",
        f0.len()
    );
    assert!(f0.iter().any(|f| *f > 0.0), "sine must have voiced frames");

    // 合成任务：首次加载全部模型，随后进入 diffusion
    engine
        .submit(2, JobKind::Synth {
            src: sine(0.5, 44_100),
            sample_rate: 44_100,
            reference_path: PathBuf::from(REFERENCE_WAV),
            params: synth_params(2),
        })
        .unwrap();
    let mut saw_loading_models = false;
    let mut saw_diffusion = false;
    let mut synth_result = None;
    loop {
        match recv(&rx) {
            Msg::State {
                job_id: 2,
                state: JobStateName::LoadingModels,
                ..
            } => saw_loading_models = true,
            Msg::State {
                job_id: 2,
                state: JobStateName::Running,
                stage,
                fraction,
                ..
            } => {
                if stage == "diffusion" && fraction > 0.0 {
                    saw_diffusion = true;
                    assert!(fraction <= 1.0);
                }
            }
            Msg::Synth {
                job_id: 2,
                audio,
                first_vocoder,
                f0,
            } => synth_result = Some((audio, first_vocoder, f0)),
            Msg::State {
                job_id: 2,
                state: JobStateName::Succeeded,
                ..
            } => break,
            Msg::State {
                job_id: 2,
                state: JobStateName::Failed,
                error,
                ..
            } => panic!("synth failed: {error:?}"),
            _ => {}
        }
    }
    assert!(saw_loading_models, "first synth must load models");
    assert!(saw_diffusion, "synth must report diffusion progress");
    let (audio, first_vocoder, f0) = synth_result.expect("synth result must arrive");
    assert!(
        audio.len().abs_diff(22_050) <= 512,
        "output length {} differs from input",
        audio.len()
    );
    assert!(audio.iter().all(|s| s.is_finite()));
    assert!(first_vocoder.is_some(), "first vocoder output requested");
    assert!(!f0.is_empty());

    // 排队与取消：合成运行期间，检测进入队列并被取消
    engine
        .submit(3, JobKind::Synth {
            src: sine(0.5, 44_100),
            sample_rate: 44_100,
            reference_path: PathBuf::from(REFERENCE_WAV),
            params: synth_params(8),
        })
        .unwrap();
    wait_state(&rx, 3, JobStateName::Running);
    engine
        .submit(4, JobKind::Detect {
            pcm: sine(0.5, 16_000),
            sample_rate: 16_000,
            estimator: Estimator::Rmvpe,
        })
        .unwrap();
    match wait_state(&rx, 4, JobStateName::Queued) {
        Msg::State {
            queue_position, ..
        } => assert_eq!(queue_position, 1, "job 4 must be first in queue"),
        _ => unreachable!(),
    }
    engine.cancel(4);
    wait_state(&rx, 4, JobStateName::Cancelled);
    wait_state(&rx, 3, JobStateName::Succeeded);

    // 失败隔离：坏路径任务失败之后，引擎继续服务
    engine
        .submit(5, JobKind::Synth {
            src: sine(0.5, 44_100),
            sample_rate: 44_100,
            reference_path: PathBuf::from("/nonexistent/timbre.wav"),
            params: synth_params(2),
        })
        .unwrap();
    match wait_state(&rx, 5, JobStateName::Failed) {
        Msg::State { error, .. } => {
            let error = error.expect("failed job must carry an error message");
            assert!(error.contains("nonexistent"), "unexpected error: {error}");
        }
        _ => unreachable!(),
    }
    engine
        .submit(6, JobKind::Detect {
            pcm: sine(0.5, 16_000),
            sample_rate: 16_000,
            estimator: Estimator::Rmvpe,
        })
        .unwrap();
    wait_state(&rx, 6, JobStateName::Succeeded);
}
