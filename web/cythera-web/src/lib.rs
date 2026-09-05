//! A raw C-ABI binding over the systemless fork for a browser host.
//!
//! No wasm-bindgen: the page talks to this module through a handful of
//! exported functions and reads pixels and audio straight out of linear
//! memory. Everything lives in one thread-local `State`; WebAssembly on the
//! web is single-threaded, so that is the whole synchronisation story.

use std::cell::RefCell;
use std::collections::HashMap;

use systemless::display::{self, DisplayGamma};
use systemless::game;
use systemless::loader::LoadedApp;
use systemless::runner::{FixtureRunner, FixtureRunnerConfig, VfsFileSnapshot, VfsFileStat};

#[link(wasm_import_module = "env")]
extern "C" {
    /// Supplied by the page as `env.cw_log`: a line of UTF-8 to print.
    fn cw_log(ptr: *const u8, len: usize);
}

fn log(msg: &str) {
    unsafe { cw_log(msg.as_ptr(), msg.len()) }
}

/// What the desktop store compares to decide a file changed: both forks'
/// lengths and hashes, the Finder info and the modification date.
type Fingerprint = (usize, usize, u64, u64, u32, u32, u16, u32);

struct State {
    runner: FixtureRunner,
    frame: Vec<u8>,
    audio: Vec<u8>,
    mouse: (i16, i16),
    width: u16,
    height: u16,
    /// The launched application, held between `cw_load` and `cw_start` so
    /// saves can be imported before the game looks for them.
    app: Option<LoadedApp>,
    /// Every persistable file's stat as it came out of the archive. A file
    /// that still matches its baseline was never touched by the guest and is
    /// not a save, whatever folder it is in.
    baseline: HashMap<String, VfsFileStat>,
    /// Fingerprints of what the page has stored. A file is offered again
    /// only when it no longer matches.
    synced: HashMap<String, Fingerprint>,
    /// Files changed since the last acknowledged scan, in the order the page
    /// reads them; `cw_save_ack` moves their fingerprints into `synced`.
    pending: Vec<(VfsFileSnapshot, Fingerprint)>,
    /// The full VFS path answered by `cw_vfs_find`.
    found: String,
    /// The JSON answered by `cw_menus`.
    menus: String,
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

/// `cw_load` then `cw_start`: for a host with no saves to restore.
#[no_mangle]
pub extern "C" fn cw_boot(
    ptr: *mut u8,
    len: usize,
    mac_epoch_secs: u32,
    width: u32,
    height: u32,
) -> i32 {
    let rc = cw_load(ptr, len, mac_epoch_secs, width, height);
    if rc != 0 {
        return rc;
    }
    cw_start()
}

/// Build the machine and load the archive at `ptr..ptr+len` (ownership
/// passes here), stopping short of the launch sequence so the page can
/// `cw_import` its stored saves first; `cw_start` then launches. `mac_epoch_secs` seeds the guest clock:
/// the runner's own fallback is `SystemTime::now()`, which panics on
/// `wasm32-unknown-unknown`. `width` and `height` are the guest screen in
/// pixels; 0 for either takes the fork's default (800x600). Cythera lays its
/// start screen out for 640x480 and lets the game run at any larger size, so
/// a phone asks for 640 wide in portrait or 480 high in landscape and fills
/// the other axis. Returns 0 on success, -1 with `cw_error_*` set.
#[no_mangle]
pub extern "C" fn cw_load(
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
    let baseline = runner
        .vfs_file_stats_where(browser_save_path)
        .into_iter()
        .map(|stat| (stat.path.clone(), stat))
        .collect();
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
            app: Some(app),
            baseline,
            synced: HashMap::new(),
            pending: Vec::new(),
            found: String::new(),
            menus: String::new(),
        })
    });
    0
}

/// Run the launch sequence. Anything imported before this is on the disk
/// the application finds. Returns -1 if nothing is loaded or it already ran.
#[no_mangle]
pub extern "C" fn cw_start() -> i32 {
    with_state(|s| match s.app.take() {
        Some(app) => {
            game::init_game(&mut s.runner, &app);
            0
        }
        None => -1,
    })
    .unwrap_or(-1)
}

// ---------------------------------------------------------------- saves
//
// The desktop runner scans the VFS every 30 frames, compares each file it
// might persist against the archive and against what it last wrote, and
// writes the ones that changed. The same policy, with the page as the
// store: `cw_save_scan` collects the changed files, the page reads each
// through the accessors and puts it in IndexedDB, and `cw_save_ack` records
// them as stored. Only files under the game's own folders and the one
// preference file are considered; Ambrosia's licence record and the log
// under Preferences are not a player's state.

fn browser_save_path(path: &str) -> bool {
    let lower = path.trim_matches('/').to_ascii_lowercase();
    if lower.is_empty()
        || lower.starts_with("__rsrc__/")
        || lower.starts_with("system folder/temporary items/")
        || lower.starts_with("temporary items/")
        || lower.starts_with("trash/")
    {
        return false;
    }
    if lower.starts_with("system folder/") {
        return lower == "system folder/preferences/cythera preferences";
    }
    true
}

fn stats_match(left: &VfsFileStat, right: &VfsFileStat) -> bool {
    left.data_len == right.data_len
        && left.resource_len == right.resource_len
        && left.file_type == right.file_type
        && left.creator == right.creator
        && left.finder_flags == right.finder_flags
        && left.modified_date == right.modified_date
}

fn fingerprint(runner: &mut FixtureRunner, path: &str) -> Option<Fingerprint> {
    let summary = runner.vfs_file_summary(path)?;
    Some((
        summary.data_len,
        summary.resource_len,
        summary.data_hash,
        summary.resource_hash,
        summary.file_type,
        summary.creator,
        summary.finder_flags,
        summary.modified_date,
    ))
}

/// Put a stored file on the disk. Buffers come from `cw_alloc` and are
/// owned here afterwards. Call between `cw_load` and `cw_start` to restore
/// saves; later calls add a file the game sees the next time it lists the
/// folder. Returns -1 when nothing is loaded.
#[no_mangle]
pub extern "C" fn cw_import(
    path_ptr: *mut u8,
    path_len: usize,
    file_type: u32,
    creator: u32,
    finder_flags: u32,
    created_date: u32,
    modified_date: u32,
    data_ptr: *mut u8,
    data_len: usize,
    rsrc_ptr: *mut u8,
    rsrc_len: usize,
) -> i32 {
    let path_bytes = unsafe { Vec::from_raw_parts(path_ptr, path_len, path_len) };
    let data_fork = if data_ptr.is_null() {
        Vec::new()
    } else {
        unsafe { Vec::from_raw_parts(data_ptr, data_len, data_len) }
    };
    let resource_fork = if rsrc_ptr.is_null() {
        Vec::new()
    } else {
        unsafe { Vec::from_raw_parts(rsrc_ptr, rsrc_len, rsrc_len) }
    };
    let path = String::from_utf8_lossy(&path_bytes).into_owned();
    with_state(|s| {
        let file = VfsFileSnapshot {
            path: path.clone(),
            data_fork,
            resource_fork,
            file_type,
            creator,
            finder_flags: finder_flags as u16,
            created_date,
            modified_date,
        };
        s.runner.import_vfs_file(&file);
        if let Some(fp) = fingerprint(&mut s.runner, &path) {
            s.synced.insert(path, fp);
        }
        0
    })
    .unwrap_or(-1)
}

/// Collect the files that changed since the last acknowledged scan and
/// return how many there are. The accessors below address them by index
/// until `cw_save_ack` or the next scan.
#[no_mangle]
pub extern "C" fn cw_save_scan() -> u32 {
    with_state(|s| {
        s.pending.clear();
        let stats = s.runner.vfs_file_stats_where(browser_save_path);
        for stat in stats {
            let untouched = !s.synced.contains_key(&stat.path)
                && s
                    .baseline
                    .get(&stat.path)
                    .is_some_and(|archive| stats_match(archive, &stat));
            if untouched {
                continue;
            }
            let Some(fp) = fingerprint(&mut s.runner, &stat.path) else {
                continue;
            };
            if s.synced.get(&stat.path) == Some(&fp) {
                continue;
            }
            if let Some(snapshot) = s.runner.vfs_file_snapshot(&stat.path) {
                s.pending.push((snapshot, fp));
            }
        }
        s.pending.len() as u32
    })
    .unwrap_or(0)
}

fn pending<R>(index: u32, f: impl FnOnce(&VfsFileSnapshot) -> R) -> Option<R> {
    with_state(|s| s.pending.get(index as usize).map(|(snap, _)| f(snap))).flatten()
}

#[no_mangle]
pub extern "C" fn cw_save_path_ptr(i: u32) -> *const u8 {
    pending(i, |f| f.path.as_ptr()).unwrap_or(std::ptr::null())
}
#[no_mangle]
pub extern "C" fn cw_save_path_len(i: u32) -> usize {
    pending(i, |f| f.path.len()).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn cw_save_type(i: u32) -> u32 {
    pending(i, |f| f.file_type).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn cw_save_creator(i: u32) -> u32 {
    pending(i, |f| f.creator).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn cw_save_flags(i: u32) -> u32 {
    pending(i, |f| u32::from(f.finder_flags)).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn cw_save_created(i: u32) -> u32 {
    pending(i, |f| f.created_date).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn cw_save_modified(i: u32) -> u32 {
    pending(i, |f| f.modified_date).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn cw_save_data_ptr(i: u32) -> *const u8 {
    pending(i, |f| f.data_fork.as_ptr()).unwrap_or(std::ptr::null())
}
#[no_mangle]
pub extern "C" fn cw_save_data_len(i: u32) -> usize {
    pending(i, |f| f.data_fork.len()).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn cw_save_rsrc_ptr(i: u32) -> *const u8 {
    pending(i, |f| f.resource_fork.as_ptr()).unwrap_or(std::ptr::null())
}
#[no_mangle]
pub extern "C" fn cw_save_rsrc_len(i: u32) -> usize {
    pending(i, |f| f.resource_fork.len()).unwrap_or(0)
}

/// The page has stored everything the last scan offered.
#[no_mangle]
pub extern "C" fn cw_save_ack() {
    with_state(|s| {
        for (snap, fp) in s.pending.drain(..) {
            s.synced.insert(snap.path, fp);
        }
    });
}

/// Find the first file on the disk with this name (the last path segment)
/// and keep its full path for `cw_found_ptr`/`cw_found_len`. The page uses
/// it to learn the game's folder from `Cythera Data` before importing a
/// character file beside it. Returns 1 when found.
#[no_mangle]
pub extern "C" fn cw_vfs_find(name_ptr: *const u8, name_len: usize) -> i32 {
    let name = unsafe { std::slice::from_raw_parts(name_ptr, name_len) };
    let name = String::from_utf8_lossy(name).to_ascii_lowercase();
    with_state(|s| {
        let hit = s
            .runner
            .vfs_file_stats_where(|p| {
                p.rsplit('/').next().map(str::to_ascii_lowercase).as_deref() == Some(name.as_str())
            })
            .into_iter()
            .next();
        match hit {
            Some(stat) => {
                s.found = stat.path;
                1
            }
            None => {
                s.found.clear();
                0
            }
        }
    })
    .unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn cw_found_ptr() -> *const u8 {
    with_state(|s| s.found.as_ptr()).unwrap_or(std::ptr::null())
}
#[no_mangle]
pub extern "C" fn cw_found_len() -> usize {
    with_state(|s| s.found.len()).unwrap_or(0)
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

/// Set the guest cycles per VBL tick. The default is the 25 MHz profile's;
/// a host that cannot deliver that many instructions per real second lowers
/// it so that ticks stay on the wall clock and the guest runs like a slower
/// Macintosh instead of falling behind the clock.
#[no_mangle]
pub extern "C" fn cw_set_instructions_per_tick(n: u32) {
    with_state(|s| s.runner.set_instructions_per_tick(n.clamp(50_000, 2_000_000)));
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

// ---------------------------------------------------------------- menus
//
// Cythera hides its menu bar and reveals it when the pointer reaches the top
// of the screen, which a finger cannot do. The runner projects the live Menu
// Manager state and routes a host-chosen item back through the guest's own
// MenuSelect path, so the page can draw the menus itself and Save, Save As
// and Quit need no key chord. Hand-rolled JSON; the shape is small.

fn json_str(out: &mut String, text: &str) {
    out.push('"');
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

/// Snapshot the guest's menus as JSON: `[{id, title, enabled, inBar, items:
/// [{n, text, enabled, checked, key, separator, submenu}]}]`. The text lives
/// until the next call into the module.
#[no_mangle]
pub extern "C" fn cw_menus() -> *const u8 {
    with_state(|s| {
        let snapshot = s.runner.guest_menu_snapshot();
        let mut out = String::from("[");
        for (mi, menu) in snapshot.menus.iter().enumerate() {
            if mi > 0 {
                out.push(',');
            }
            out.push_str(&format!("{{\"id\":{},\"title\":", menu.id));
            json_str(&mut out, &menu.title);
            out.push_str(&format!(
                ",\"enabled\":{},\"inBar\":{},\"items\":[",
                menu.enabled,
                menu.visible_in_menu_bar && !menu.hierarchical
            ));
            for (ii, item) in menu.items.iter().enumerate() {
                if ii > 0 {
                    out.push(',');
                }
                out.push_str(&format!("{{\"n\":{},\"text\":", item.number));
                json_str(&mut out, &item.text);
                out.push_str(&format!(
                    ",\"enabled\":{},\"checked\":{},\"separator\":{},\"submenu\":{},\"key\":",
                    item.enabled,
                    item.checked,
                    item.separator,
                    item.submenu_id.map_or("null".to_string(), |id| id.to_string())
                ));
                match item.key_equivalent {
                    Some(k) => json_str(&mut out, &k.to_string()),
                    None => out.push_str("null"),
                }
                out.push('}');
            }
            out.push_str("]}");
        }
        out.push(']');
        s.menus = out;
        s.menus.as_ptr()
    })
    .unwrap_or(std::ptr::null())
}

#[no_mangle]
pub extern "C" fn cw_menus_len() -> usize {
    with_state(|s| s.menus.len()).unwrap_or(0)
}

/// Choose a menu item as the guest would from its menu bar. Returns 1 when
/// the item was present, enabled and selectable.
#[no_mangle]
pub extern "C" fn cw_menu_select(menu_id: i32, item: i32) -> i32 {
    with_state(|s| i32::from(s.runner.select_guest_menu_item(menu_id as i16, item as i16)))
        .unwrap_or(0)
}
