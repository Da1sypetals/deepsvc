use crate::engine::{self, Estimator, Event, JobKind, JobStateName, SynthParams};
use std::ffi::{CStr, CString, c_char, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::{Arc, Once};

// 与 plugin/Source/Engine/EngineBridge.cpp 中的定义一一对应
pub const EVENT_JOB_STATE: u32 = 0;
pub const EVENT_DETECT_RESULT: u32 = 1;
pub const EVENT_SYNTH_RESULT: u32 = 2;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiSynthParams {
    pub f0_estimator: u32, // 0=rmvpe 1=fcpe
    pub diffusion_steps: u32,
    pub pitch_shift: f32,
    pub cfg_rate: f32,
    pub input_gain_db: f32,
    pub keep_first_vocoder_output: bool,
}

// 所有指针仅在回调执行期间有效，接收方需要拷贝
#[repr(C)]
pub struct FfiEvent {
    pub job_id: u64,
    pub kind: u32,
    pub state: u32,
    pub queue_position: u32,
    pub fraction: f64,
    pub stage: *const c_char,
    pub error: *const c_char,
    pub f0: *const f32,
    pub f0_len: u64,
    pub audio: *const f32,
    pub audio_len: u64,
    pub first_vocoder: *const f32,
    pub first_vocoder_len: u64,
}

pub type EventCallback = extern "C" fn(event: *const FfiEvent, user_data: *mut c_void);

fn state_code(state: JobStateName) -> u32 {
    match state {
        JobStateName::Queued => 0,
        JobStateName::LoadingModels => 1,
        JobStateName::Running => 2,
        JobStateName::Succeeded => 3,
        JobStateName::Failed => 4,
        JobStateName::Cancelled => 5,
    }
}

fn c_string(text: &str) -> CString {
    CString::new(text).unwrap_or_default()
}

fn make_listener(callback: EventCallback, user_data: usize) -> engine::Listener {
    Arc::new(move |event: &Event| {
        let user_data = user_data as *mut c_void;
        match event {
            Event::JobState {
                job_id,
                state,
                queue_position,
                stage,
                fraction,
                error,
            } => {
                let stage = c_string(stage);
                let error_text = c_string(error.as_deref().unwrap_or(""));
                let ffi = FfiEvent {
                    job_id: *job_id,
                    kind: EVENT_JOB_STATE,
                    state: state_code(*state),
                    queue_position: *queue_position,
                    fraction: *fraction,
                    stage: stage.as_ptr(),
                    error: if error.is_some() {
                        error_text.as_ptr()
                    } else {
                        std::ptr::null()
                    },
                    f0: std::ptr::null(),
                    f0_len: 0,
                    audio: std::ptr::null(),
                    audio_len: 0,
                    first_vocoder: std::ptr::null(),
                    first_vocoder_len: 0,
                };
                callback(&ffi, user_data);
            }
            Event::DetectResult { job_id, f0 } => {
                let ffi = FfiEvent {
                    job_id: *job_id,
                    kind: EVENT_DETECT_RESULT,
                    state: 0,
                    queue_position: 0,
                    fraction: -1.0,
                    stage: std::ptr::null(),
                    error: std::ptr::null(),
                    f0: f0.as_ptr(),
                    f0_len: f0.len() as u64,
                    audio: std::ptr::null(),
                    audio_len: 0,
                    first_vocoder: std::ptr::null(),
                    first_vocoder_len: 0,
                };
                callback(&ffi, user_data);
            }
            Event::SynthResult {
                job_id,
                audio,
                first_vocoder,
                f0,
            } => {
                let empty: Vec<f32> = Vec::new();
                let first_vocoder = first_vocoder.as_deref().unwrap_or(&empty);
                let ffi = FfiEvent {
                    job_id: *job_id,
                    kind: EVENT_SYNTH_RESULT,
                    state: 0,
                    queue_position: 0,
                    fraction: -1.0,
                    stage: std::ptr::null(),
                    error: std::ptr::null(),
                    f0: f0.as_ptr(),
                    f0_len: f0.len() as u64,
                    audio: audio.as_ptr(),
                    audio_len: audio.len() as u64,
                    first_vocoder: first_vocoder.as_ptr(),
                    first_vocoder_len: first_vocoder.len() as u64,
                };
                callback(&ffi, user_data);
            }
        }
    })
}

fn into_c_string(text: String) -> *mut c_char {
    CString::new(text).unwrap_or_default().into_raw()
}

fn init_logger() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let mut builder =
            env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"));
        if let Some(path) = std::env::var_os("DEEPSVC_LOG_FILE")
            && let Ok(file) = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(&path)
        {
            builder.target(env_logger::Target::Pipe(Box::new(file)));
        }
        let _ = builder.try_init();
    });
}

// 返回 NULL 表示成功，否则返回错误字符串（调用方用 deepsvc_engine_free_string 释放）
#[unsafe(no_mangle)]
pub extern "C" fn deepsvc_engine_set_model_dir(path: *const c_char) -> *mut c_char {
    init_logger();
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<(), String> {
        if path.is_null() {
            return Err("model dir path is null".to_string());
        }
        let path = unsafe { CStr::from_ptr(path) }
            .to_str()
            .map_err(|err| err.to_string())?;
        engine::shared()
            .set_model_dir(PathBuf::from(path))
            .map_err(|err| err.to_string())
    }));
    match result {
        Ok(Ok(())) => std::ptr::null_mut(),
        Ok(Err(message)) => into_c_string(message),
        Err(_) => into_c_string("engine panicked".to_string()),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn deepsvc_engine_free_string(text: *mut c_char) {
    if !text.is_null() {
        drop(unsafe { CString::from_raw(text) });
    }
}

// 返回监听器句柄，0 表示失败
#[unsafe(no_mangle)]
pub extern "C" fn deepsvc_engine_listener_add(
    callback: Option<EventCallback>,
    user_data: *mut c_void,
) -> u64 {
    let Some(callback) = callback else { return 0 };
    engine::shared().add_listener(make_listener(callback, user_data as usize))
}

#[unsafe(no_mangle)]
pub extern "C" fn deepsvc_engine_listener_remove(handle: u64) {
    engine::shared().remove_listener(handle);
}

// 返回 0 表示已入队；失败时返回 -1 并广播该任务的 Failed 事件
#[unsafe(no_mangle)]
pub extern "C" fn deepsvc_engine_submit_detect(
    job_id: u64,
    pcm: *const f32,
    pcm_len: u64,
    sample_rate: u32,
    estimator: u32,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if pcm.is_null() || pcm_len == 0 {
            return -1;
        }
        let estimator = match estimator {
            0 => Estimator::Rmvpe,
            1 => Estimator::Fcpe,
            _ => return -1,
        };
        let samples: Arc<[f32]> =
            unsafe { std::slice::from_raw_parts(pcm, pcm_len as usize) }.into();
        submit_or_fail(job_id, JobKind::Detect {
            pcm: samples,
            sample_rate,
            estimator,
        })
    }))
    .unwrap_or(-1)
}

#[unsafe(no_mangle)]
pub extern "C" fn deepsvc_engine_submit_synth(
    job_id: u64,
    pcm: *const f32,
    pcm_len: u64,
    sample_rate: u32,
    reference_path: *const c_char,
    params: FfiSynthParams,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if pcm.is_null() || pcm_len == 0 || reference_path.is_null() {
            return -1;
        }
        let f0_estimator = match params.f0_estimator {
            0 => Estimator::Rmvpe,
            1 => Estimator::Fcpe,
            _ => return -1,
        };
        let Ok(reference_path) = unsafe { CStr::from_ptr(reference_path) }.to_str() else {
            return -1;
        };
        let samples: Arc<[f32]> =
            unsafe { std::slice::from_raw_parts(pcm, pcm_len as usize) }.into();
        submit_or_fail(job_id, JobKind::Synth {
            src: samples,
            sample_rate,
            reference_path: PathBuf::from(reference_path),
            params: SynthParams {
                f0_estimator,
                diffusion_steps: params.diffusion_steps,
                pitch_shift: params.pitch_shift,
                cfg_rate: params.cfg_rate,
                input_gain_db: params.input_gain_db,
                keep_first_vocoder_output: params.keep_first_vocoder_output,
            },
        })
    }))
    .unwrap_or(-1)
}

#[unsafe(no_mangle)]
pub extern "C" fn deepsvc_engine_cancel(job_id: u64) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        engine::shared().cancel(job_id);
    }));
}

fn submit_or_fail(job_id: u64, kind: JobKind) -> i32 {
    let engine = engine::shared();
    match engine.submit(job_id, kind) {
        Ok(()) => 0,
        Err(err) => {
            engine.emit(&Event::job_state(
                job_id,
                JobStateName::Failed,
                0,
                "",
                -1.0,
                Some(format!("{err:#}")),
            ));
            -1
        }
    }
}
