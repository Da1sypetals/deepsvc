use anyhow::{Result, bail};
use std::collections::{HashMap, VecDeque};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::thread;

use crate::worker;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Estimator {
    Rmvpe,
    Fcpe,
}

#[derive(Clone, Copy, Debug)]
pub struct SynthParams {
    pub f0_estimator: Estimator,
    pub diffusion_steps: u32,
    pub pitch_shift: f32,
    pub cfg_rate: f32,
    pub input_gain_db: f32,
    pub keep_first_vocoder_output: bool,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum JobStateName {
    Queued,
    LoadingModels,
    Running,
    Succeeded,
    Failed,
    Cancelled,
}

// 引擎事件：任务状态变化与任务结果，广播给全部监听器
pub enum Event {
    JobState {
        job_id: u64,
        state: JobStateName,
        queue_position: u32,
        stage: String,
        fraction: f64,
        error: Option<String>,
    },
    DetectResult {
        job_id: u64,
        f0: Arc<Vec<f32>>,
    },
    SynthResult {
        job_id: u64,
        audio: Arc<Vec<f32>>,
        first_vocoder: Option<Arc<Vec<f32>>>,
        f0: Arc<Vec<f32>>,
    },
}

impl Event {
    pub fn job_state(
        job_id: u64,
        state: JobStateName,
        queue_position: u32,
        stage: &str,
        fraction: f64,
        error: Option<String>,
    ) -> Event {
        Event::JobState {
            job_id,
            state,
            queue_position,
            stage: stage.to_string(),
            fraction,
            error,
        }
    }
}

pub type Listener = Arc<dyn Fn(&Event) + Send + Sync>;

pub enum JobKind {
    Detect {
        pcm: Arc<[f32]>,
        sample_rate: u32,
        estimator: Estimator,
    },
    Synth {
        src: Arc<[f32]>,
        sample_rate: u32,
        reference_path: PathBuf,
        params: SynthParams,
    },
}

pub struct Job {
    pub id: u64,
    pub kind: JobKind,
    pub cancel: Arc<AtomicBool>,
}

struct State {
    queue: VecDeque<Job>,
    listeners: HashMap<u64, Listener>,
    next_listener_id: u64,
    running: Option<Arc<AtomicBool>>,
    running_owner: Option<u64>,
    model_dir: Option<PathBuf>,
    worker_started: bool,
}

// 进程级共享引擎：模型加载一次驻留内存，全部插件实例的任务经同一队列串行执行
pub struct Engine {
    state: Mutex<State>,
    job_available: Condvar,
}

static ENGINE: OnceLock<Arc<Engine>> = OnceLock::new();

pub fn shared() -> Arc<Engine> {
    Arc::clone(ENGINE.get_or_init(|| {
        Arc::new(Engine {
            state: Mutex::new(State {
                queue: VecDeque::new(),
                listeners: HashMap::new(),
                next_listener_id: 1,
                running: None,
                running_owner: None,
                model_dir: None,
                worker_started: false,
            }),
            job_available: Condvar::new(),
        })
    }))
}

impl Engine {
    // 模型目录只能设置一次；重复设置相同路径为幂等成功
    pub fn set_model_dir(&self, dir: PathBuf) -> Result<()> {
        let mut state = self.state.lock().expect("state poisoned");
        match &state.model_dir {
            Some(existing) if *existing != dir => {
                bail!("model dir already set to {}", existing.display())
            }
            Some(_) => Ok(()),
            None => {
                state.model_dir = Some(dir);
                Ok(())
            }
        }
    }

    pub fn model_dir(&self) -> Option<PathBuf> {
        self.state.lock().expect("state poisoned").model_dir.clone()
    }

    pub fn add_listener(&self, listener: Listener) -> u64 {
        let mut state = self.state.lock().expect("state poisoned");
        let id = state.next_listener_id;
        state.next_listener_id += 1;
        state.listeners.insert(id, listener);
        id
    }

    pub fn remove_listener(&self, id: u64) {
        self.state
            .lock()
            .expect("state poisoned")
            .listeners
            .remove(&id);
    }

    pub fn emit(&self, event: &Event) {
        // 先快照再回调：回调内允许增删监听器、提交或取消任务
        let listeners: Vec<Listener> = {
            let state = self.state.lock().expect("state poisoned");
            state.listeners.values().cloned().collect()
        };
        for listener in listeners {
            listener(event);
        }
    }

    pub fn submit(self: &Arc<Self>, job_id: u64, kind: JobKind) -> Result<()> {
        let mut state = self.state.lock().expect("state poisoned");
        if state.model_dir.is_none() {
            bail!("model dir is not set");
        }
        if !state.worker_started {
            state.worker_started = true;
            let engine = Arc::clone(self);
            thread::spawn(move || worker::run_worker(engine));
        }
        state.queue.push_back(Job {
            id: job_id,
            kind,
            cancel: Arc::new(AtomicBool::new(false)),
        });
        drop(state);
        self.job_available.notify_one();
        self.notify_queue_positions();
        Ok(())
    }

    pub fn cancel(&self, job_id: u64) {
        let mut state = self.state.lock().expect("state poisoned");
        if let Some(index) = state.queue.iter().position(|job| job.id == job_id) {
            state.queue.remove(index);
            drop(state);
            self.emit(&Event::job_state(
                job_id,
                JobStateName::Cancelled,
                0,
                "",
                -1.0,
                None,
            ));
            self.notify_queue_positions();
            return;
        }
        if state.running_owner == Some(job_id)
            && let Some(cancel) = &state.running
        {
            cancel.store(true, Ordering::Relaxed);
        }
    }

    pub fn pop_job_blocking(&self) -> Job {
        let mut state = self.state.lock().expect("state poisoned");
        loop {
            if let Some(job) = state.queue.pop_front() {
                state.running = Some(Arc::clone(&job.cancel));
                state.running_owner = Some(job.id);
                return job;
            }
            state = self.job_available.wait(state).expect("state poisoned");
        }
    }

    pub fn finish_running(&self) {
        let mut state = self.state.lock().expect("state poisoned");
        state.running = None;
        state.running_owner = None;
    }

    pub fn notify_queue_positions(&self) {
        let updates: Vec<(u64, u32)> = {
            let state = self.state.lock().expect("state poisoned");
            state
                .queue
                .iter()
                .enumerate()
                .map(|(index, job)| (job.id, index as u32 + 1))
                .collect()
        };
        for (job_id, position) in updates {
            self.emit(&Event::job_state(
                job_id,
                JobStateName::Queued,
                position,
                "",
                -1.0,
                None,
            ));
        }
    }
}
