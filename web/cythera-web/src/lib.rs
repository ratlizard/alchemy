//! A raw C-ABI binding over the systemless fork for a browser host.
//!
//! No wasm-bindgen: the page talks to this module through a handful of
//! exported functions and reads pixels and audio straight out of linear
//! memory. Everything lives in one thread-local `State`; WebAssembly on the
//! web is single-threaded, so that is the whole synchronisation story.

use std::cell::RefCell;

use systemless::display::{self, DisplayGamma};
use systemless::game;
use systemless::runner::{FixtureRunner, FixtureRunnerConfig};

#[link(wasm_import_module = "env")]
extern "C" {
    /// Supplied by the page as `env.cw_log`: a line of UTF-8 to print.
    fn cw_log(ptr: *const u8, len: usize);
}

fn log(msg: &str) {
    unsafe { cw_log(msg.as_ptr(), msg.len()) }
}

struct State {
    runner: FixtureRunner,
    frame: Vec<u8>,
    audio: Vec<u8>,
    mouse: (i16, i16),
    width: u16,
    height: u16,
}

thread_local! {
    static STATE: RefCell<Option<State>> = const { RefCell::new(None) };
    static ERROR: RefCell<String> = const { RefCell::new(String::new()) };
}

fn with_state<R>(f: impl FnOnce(&mut State) -> R) -> Option<R> {
    STATE.with(|s| s.borrow_mut().as_mut().map(f))
}

fn set_error(msg: String) {
    log(&msg);
    ERROR.with(|e| *e.borrow_mut() = msg);
}

/// Route Rust panics to the page's console instead of a bare `unreachable`.
#[no_mangle]
pub extern "C" fn cw_init() {
    std::panic::set_hook(Box::new(|info| {
        let msg = format!("panic: {info}");
        log(&msg);
        ERROR.with(|e| *e.borrow_mut() = msg);
    }));
}

/// Allocate `len` bytes the page can fill (the game archive).
#[no_mangle]
pub extern "C" fn cw_alloc(len: usize) -> *mut u8 {
    let mut v = Vec::<u8>::with_capacity(len);
    let p = v.as_mut_ptr();
    std::mem::forget(v);
    p
}

/// Free a buffer from `cw_alloc` that was not handed to `cw_boot`.
#[no_mangle]
pub extern "C" fn cw_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() {
        unsafe { drop(Vec::from_raw_parts(ptr, 0, len)) };
    }
}

#[no_mangle]
pub extern "C" fn cw_error_ptr() -> *const u8 {
    ERROR.with(|e| e.borrow().as_ptr())
}

#[no_mangle]
pub extern "C" fn cw_error_len() -> usize {
    ERROR.with(|e| e.borrow().len())
}

/// Build the machine, load the archive at `ptr..ptr+len` (ownership passes
/// here) and run the launch sequence. `mac_epoch_secs` seeds the guest clock:
/// the runner's own fallback is `SystemTime::now()`, which panics on
/// `wasm32-unknown-unknown`. `width` and `height` are the guest screen in
/// pixels; 0 for either takes the fork's default (800x600). Cythera lays its
/// start screen out for 640x480 and lets the game run at any larger size, so
/// a phone asks for 640 wide in portrait or 480 high in landscape and fills
/// the other axis. Returns 0 on success, -1 with `cw_error_*` set.
#[no_mangle]
pub extern "C" fn cw_boot(
    ptr: *mut u8,
    len: usize,
    mac_epoch_secs: u32,
    width: u32,
    height: u32,
) -> i32 {
    let bytes = unsafe { Vec::from_raw_parts(ptr, len, len) };
    // `game::new_runner`'s configuration, with the theme named: upstream's
    // library default became its own chrome in 0.33, and this front end shows
    // the classic look every Cythera reference capture was taken against.
    // Set through the config rather than a setter so the crate builds against
    // both the 0.30 line and the 0.34 rebase.
    let config = FixtureRunnerConfig {
        load_address: 0x10000,
        max_instructions: game::MAX_INSTRUCTIONS_PER_FRAME,
        addressing_32_bit: true,
        ui_theme: systemless::ui_theme::UiThemeId::ClassicSystem7,
        screen_size: (width > 0 && height > 0)
            .then(|| (width.min(4096) as u16, height.min(4096) as u16)),
        ..FixtureRunnerConfig::default()
    };
    let mut runner = FixtureRunner::new(game::RAM_SIZE as usize, config);
    runner.set_app_start_time(mac_epoch_secs);
    runner.set_instructions_per_tick(
        systemless::runner::default_realtime_instructions_per_tick(false),
    );
    let app = match game::load_game(&mut runner, &bytes) {
        Ok(app) => app,
        Err(e) => {
            set_error(format!("load_game: {e}"));
            return -1;
        }
    };
    drop(bytes);
    game::init_game(&mut runner, &app);
    let (_, _, width, height, _) = runner.dispatcher().screen_mode;
    let frame = vec![0u8; usize::from(width) * usize::from(height) * 4];
    STATE.with(|s| {
        *s.borrow_mut() = Some(State {
            runner,
            frame,
            audio: Vec::new(),
            mouse: (0, 0),
            width,
            height,
        })
    });
    0
}

#[no_mangle]
pub extern "C" fn cw_width() -> u32 {
    with_state(|s| u32::from(s.width)).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn cw_height() -> u32 {
    with_state(|s| u32::from(s.height)).unwrap_or(0)
}

/// One wall-clock-paced slice: the desktop runner's `run_gui_slice_with_audio`.
/// The page converts elapsed time into `deadline_tick` and `audio_samples`
/// (22,050 Hz mono). Returns the instructions executed.
#[no_mangle]
pub extern "C" fn cw_run(max_steps: u32, deadline_tick: u32, audio_samples: u32) -> u32 {
    with_state(|s| {
        let (steps, _) = s.runner.run_gui_slice_with_audio(
            max_steps as usize,
            deadline_tick,
            audio_samples as usize,
        );
        steps as u32
    })
    .unwrap_or(0)
}

/// Headless-style run: ticks advance from the instruction count alone, no
/// wall clock. This is what makes the browser number comparable with the
/// native `--headless --max-instructions` figure.
#[no_mangle]
pub extern "C" fn cw_run_headless(max_steps: u32) -> u32 {
    with_state(|s| s.runner.run_steps(max_steps as usize, None).0 as u32).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn cw_running() -> i32 {
    with_state(|s| i32::from(!s.runner.is_halted())).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn cw_tick() -> u32 {
    with_state(|s| s.runner.guest_tick()).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn cw_instructions_per_tick() -> u32 {
    with_state(|s| s.runner.instructions_per_tick()).unwrap_or(0)
}

/// Height of the guest menu bar the page should leave visible, 0 when the
/// guest has hidden it.
#[no_mangle]
pub extern "C" fn cw_menu_bar_visible() -> i32 {
    with_state(|s| i32::from(s.runner.menu_bar_visible())).unwrap_or(0)
}

/// Render the screen, cursor included, into the RGBA frame and return its
/// address. `cw_width() * cw_height() * 4` bytes long, valid until the next
/// call into the module.
#[no_mangle]
pub extern "C" fn cw_render() -> *const u8 {
    with_state(|s| {
        let State {
            runner,
            frame,
            mouse,
            width,
            height,
            ..
        } = s;
        let screen_mode = runner.dispatcher().screen_mode;
        let clut = *runner.dispatcher().device_clut;
        let gamma: DisplayGamma = *runner.dispatcher().device_gamma;
        display::render_screen_into_with_gamma(runner.bus(), screen_mode, &clut, &gamma, frame);
        let d = runner.dispatcher();
        if d.cursor_visible() {
            if let Some(cursor) = d.cursor() {
                display::render_cursor(
                    frame,
                    u32::from(*width),
                    u32::from(*height),
                    cursor,
                    *mouse,
                );
            }
        }
        frame.as_ptr()
    })
    .unwrap_or(std::ptr::null())
}

#[no_mangle]
pub extern "C" fn cw_mouse_move(v: i32, h: i32) {
    with_state(|s| {
        s.mouse = (v as i16, h as i16);
        s.runner.set_mouse_position(v as i16, h as i16);
        s.runner.dispatcher_mut().show_cursor();
    });
}

#[no_mangle]
pub extern "C" fn cw_mouse_down(v: i32, h: i32) {
    with_state(|s| {
        s.mouse = (v as i16, h as i16);
        s.runner.push_mouse_down(v as i16, h as i16);
    });
}

#[no_mangle]
pub extern "C" fn cw_mouse_up(v: i32, h: i32) {
    with_state(|s| {
        s.mouse = (v as i16, h as i16);
        s.runner.push_mouse_up(v as i16, h as i16);
    });
}

#[no_mangle]
pub extern "C" fn cw_key_down(mac_key: u32, char_code: u32) {
    with_state(|s| s.runner.push_key_down(mac_key as u8, char_code as u8));
}

#[no_mangle]
pub extern "C" fn cw_key_up(mac_key: u32, char_code: u32) {
    with_state(|s| s.runner.push_key_up(mac_key as u8, char_code as u8));
}

/// Move the mixed audio out of the runner. Unsigned 8-bit mono PCM at
/// 22,050 Hz, silence at 0x80; `cw_audio_len()` bytes at the returned address.
#[no_mangle]
pub extern "C" fn cw_audio_drain() -> *const u8 {
    with_state(|s| {
        let State { runner, audio, .. } = s;
        runner.drain_audio_into(audio);
        audio.as_ptr()
    })
    .unwrap_or(std::ptr::null())
}

#[no_mangle]
pub extern "C" fn cw_audio_len() -> usize {
    with_state(|s| s.audio.len()).unwrap_or(0)
}
