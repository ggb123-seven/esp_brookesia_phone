# Classroom Schedule Contract

## 1. Scope / Trigger

Use this contract when changing the classroom schedule firmware client, the
Windows middleware, EAMS session handling, classroom catalogs, mock/fixture
data, or the app-local LVGL font. The catalog and schedule APIs form one
cross-layer path and must be validated together.

## 2. Signatures

- Health: `GET /health`
- Building catalog:
  `GET /classroom-schedule/buildings?token=<secret>`
- Room catalog:
  `GET /classroom-schedule/rooms?building=<UTF-8 display name>&token=<secret>`
- Schedule:
  `GET /classroom-schedule/today?classroom=<full UTF-8 room name>&date=YYYY-MM-DD&token=<secret>`
- Provider methods:

```python
ScheduleProvider.list_buildings() -> list[str]
ScheduleProvider.list_rooms(building_name: str) -> list[str]
ScheduleProvider.fetch_schedule(classroom: str, date_text: str) -> dict[str, Any]
```

- Firmware paths are independently configured by
  `CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_BUILDINGS_PATH`,
  `CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_ROOMS_PATH`, and
  `CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_API_PATH`.

## 3. Contracts

The middleware discovers buildings from the current EAMS
`class-details.action` page. Each usable `room.building.id` option becomes a
`BuildingCatalogEntry(name, building_id)`. It fetches that building's complete
room names with only `semesterId`, `iWeek`, and `room.building.id`; do not send
or infer the obsolete `buildingname` parameter.

Building catalog HTTP 200:

```json
{"buildings":["博文楼","知行楼","尔雅楼"],"updated_at":"2026-07-17T18:00:00+08:00"}
```

Room catalog HTTP 200:

```json
{"building":"尔雅楼","rooms":["尔雅楼101","尔雅楼102"],"updated_at":"2026-07-17T18:00:00+08:00"}
```

Catalog order is stable, empty placeholders are removed, duplicates are
removed without reordering, and a legal building with no rooms returns
`rooms: []`. A catalog name must occupy at most 63 UTF-8 bytes so it fits the
firmware's NUL-terminated `char[64]`; Python character count is not the
cross-layer limit. The middleware caches buildings and per-building rooms for
a bounded TTL under one catalog lock. Refreshing the building generation
invalidates all per-building room caches and the room-to-building index.

A schedule HTTP 200 contains matching `classroom` and `date`, a
`classroom_name` for the same target, `updated_at`, and `courses`. Each course
contains `start`, `end`, `name`, `teacher`, and `group`. The EAMS provider must
resolve the complete room name through the current catalog before querying;
when room occupancy data has no matching syllabus/detail row, emit an
occupancy-only course with `name` like `仅占用：第1-2节`, `teacher` set to
`教室占用`, and `group` set to `EAMS未匹配课程信息` so the firmware does not display
it as a fully identified lesson.
after one forced catalog refresh, a missing room is HTTP 400 rather than an
empty successful schedule.
Values that are not already in the room-to-building index and do not start
with a known building name, such as old fixture rooms like `A101`, must fail
fast with HTTP 400. Do not scan every building for these values because one
slow EAMS room-catalog request can turn an invalid room into a long device
timeout.
For a valid room/date that already has a fresh successful server cache entry,
the middleware may return the cached schedule before re-querying the slow EAMS
syllabus/occupancy pages, but it must first validate that the room still maps
to the current catalog target. Provider 4xx/`BadRequest` results must not be
hidden by cache; provider unavailable/upstream failures may still use the
matching cache entry. When no cache entry exists, the EAMS syllabus and room
occupancy pages should be fetched in parallel after classroom resolution so a
single device request is not delayed by two serial upstream paths.

The firmware stores building and classroom in one NVS commit. It keeps saved
configuration, dropdown drafts, and received schedule data separate. Every
catalog or schedule request carries a monotonically increasing generation plus
its server/building/classroom/date snapshot. While one worker is active, only
the latest pending request is retained. A stale result cannot update dropdowns,
UI, or the schedule cache. Completion must be signaled before a pending worker
is launched, and an old worker must not clear the new task handle.

Offline schedule cache reuse requires the same server host, classroom, and
date. Only transport failures or HTTP 5xx may fall back to cache; HTTP 4xx is a
configuration/authentication result and must remain visible. Catalog responses
are never synthesized from schedule cache.

Cookie values, API tokens, storage state, JSESSIONID values, full query strings,
and complete WebVPN URLs stay in ignored local files and sanitized logs.

## 4. Validation & Error Matrix

| Condition | Middleware result | Firmware behavior |
| --- | --- | --- |
| Missing/invalid token | HTTP 401 | Show authentication/configuration error; no cache fallback |
| Missing, overlong, or unknown building | HTTP 400 | Preserve NVS selection and require a valid catalog choice |
| Unknown room after forced refresh | HTTP 400 | Show invalid selection; never display an empty schedule |
| EAMS login page, redirect, 401, or 403 | HTTP 503 | Ask for EAMS re-login; schedule cache may be shown if the snapshot matches |
| Other upstream network/parse failure | HTTP 502/503 | Show retryable error or matching schedule cache |
| Legal building with zero rooms | HTTP 200 and `rooms: []` | Disable room save and show the zero-room state |
| Room response building mismatch | HTTP 200 rejected locally | Keep the current room list unchanged |
| Response generation/snapshot mismatch | Result discarded locally | Run only the latest pending request |
| Valid empty `courses` | HTTP 200 | Show explicit no-course state |
| Catalog or JSON exceeds firmware bounds | HTTP 413 or local size error | Show catalog/response-too-large error |

EAMS login responses must terminate catalog or syllabus scanning immediately.
Do not continue through every building/page while holding the provider lock.

## 5. Good / Base / Bad Cases

- Good: select two rooms in one building and one room in another building;
  every response target matches exactly and the returned schedules follow the
  selected rooms.
- Base: a valid building returns zero rooms, or a valid room/date returns
  `courses: []`; both remain successful but distinct UI states.
- Bad: source constants map display names to EAMS IDs, a late response replaces
  a newer draft, a 400 response displays old cached courses, or a worker signals
  completion and then clears a newly created task handle.

## 6. Tests Required

- Run `python -B -X utf8 -m py_compile` for both schedule servers.
- Run `real_classroom_schedule_server.py --self-test`; assert catalog
  200/400/401/404/503 behavior, existing schedule fields, and that cached data
  is used for provider/5xx failure but not for `BadRequest`.
- Start the mock server and call all three APIs; assert UTF-8 building/room
  round-trips, exact `classroom`/`date`, and 401/400/404 errors.
- With an ignored current session, list real buildings, list at least two
  buildings' rooms, and query two same-building rooms plus one cross-building
  room. Log only counts and exact-target booleans.
- Search the real provider for static building ID/query-name tables and
  `buildingname`; none may remain on the query path.
- Compare app copy plus mock/fixture and current real catalog CJK characters
  with `classroom_schedule_font_20.c`; assert `missing=0` without changing the
  UTF-8 query value for future unknown glyphs.
- Run `git diff --check`, `idf.py build`, flash COM3, then test rapid selection,
  close/reopen, NVS restoration, same/cross-building rooms, and error states.

## 7. Wrong vs Correct

Wrong:

```python
building_id = EAMS_BUILDING_IDS[displayed_building]
query["buildingname"] = EAMS_BUILDING_QUERY_NAMES[displayed_building]
```

Correct:

```python
entry = next(item for item in provider._buildings if item.name == displayed_building)
query["room.building.id"] = entry.building_id
```

Wrong:

```cpp
_busy = false;
xSemaphoreGive(_worker_done);
_worker_task = NULL; // Can erase the handle of a pending worker.
```

Correct:

```cpp
xSemaphoreGive(_worker_done);
_busy = false;
startPendingRequest(); // The old worker never clears the replacement handle.
```

## 8. Windows Session Refresh Launcher

### 1. Scope / Trigger

Use this contract when changing the Windows launcher, EAMS browser login,
Playwright storage state, local schedule token wiring, or managed server
restart behavior. The user-facing workflow must remain a single desktop batch
entry even when internal helpers are separate modules.

### 2. Signatures

- Repository entry:
  `classroom_schedule_server_scripts/start_classroom_schedule_server.bat [--check-only|--autostart]`
- PowerShell orchestrator:
  `classroom_schedule_server_scripts/start_classroom_schedule_server.ps1 [-CheckOnly] [-AutoStart] [-SkipSessionCheck]`
- Browser helper:
  `python tools/eams_session_refresh.py --session-file <path> --login-file <path> --profile-dir <path> --timeout-seconds <30..3600>`
- Refresh settings: `EAMS_BROWSER_PROFILE`, `EAMS_LOGIN_TIMEOUT_SECONDS`,
  `EDGE_EXECUTABLE`, and `REFRESH_EAMS_SESSION`.
- The desktop `SCHEDULE_API_TOKEN` must equal firmware
  `CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_TOKEN`; compare values without logging
  either value.
- Real launcher configuration lives only in
  `.local-secrets/classroom_schedule_server_config.bat`; the tracked example
  beside the launcher contains placeholders only.

### 3. Contracts

- Probe the current storage state before starting. A valid state starts or
  reuses the server without opening Edge.
- An expired/rejected state opens visible Edge, fills username/password from
  the ignored login file, and waits for the user to complete the slider and
  submit login. Never click, drag, recognize, or bypass the CAPTCHA/slider.
- Treat an EAMS URL or recognized EAMS page title only as a candidate. Export
  to a temporary storage-state file, run the existing sanitized EAMS probe,
  then atomically replace the configured state only after a valid result.
- Failed, closed, or timed-out login preserves the previous storage state and
  leaves an existing server process running.
- Restart an existing server after refresh only when both the listener command
  line identifies this project's `real_classroom_schedule_server.py` and its
  parent identifies this project's PowerShell launcher. Recheck immediately
  before stopping the listener.
- `--check-only` never opens Edge, writes storage state, or restarts a process.
- Capture Python helper stdout/stderr directly from `Diagnostics.Process` and
  parse its JSON result. Do not rely on `Start-Process` temporary-file output;
  nested PowerShell launchers can observe an empty file after the child exits.
- Sanitized URLs must remove query/fragment data and every
  `;jsessionid=<value>` path parameter.

### 4. Validation & Error Matrix

| Condition | Launcher result |
| --- | --- |
| Current EAMS session valid | Start server, or reuse a healthy managed instance |
| Session expired/rejected | Open visible Edge when refresh is enabled |
| School network unavailable | Report reachability failure; do not open login as an expiry fallback |
| Slider/login not completed in 600 seconds | Close owned browser, preserve old state, exit |
| Candidate state fails EAMS probe | Delete candidate, preserve old state, keep waiting until timeout |
| Port owned by an unrelated process | Report PID and exit without terminating it |
| Managed listener fails `/health` | Report unhealthy instance; do not claim it is reusable |
| Desktop token differs from firmware token | Device receives HTTP 401; align ignored desktop config and restart server |
| Helper output missing/invalid JSON | Run a sanitized session probe as recovery; fail if the state is not valid |

### 5. Good / Base / Bad Cases

- Good: an expired state opens Edge, the user completes the slider, the
  candidate returns EAMS HTTP 200, storage state is replaced, and the verified
  old listener restarts with the new session.
- Base: the state is already valid and a healthy managed listener exists;
  repeated launch exits successfully without changing the PID.
- Bad: the launcher automates the slider, overwrites the final state before a
  real probe, kills an arbitrary port owner, or accepts `/health` while the
  firmware token still receives HTTP 401.

### 6. Tests Required

- Run `eams_session_refresh.py --self-test`; assert rejected candidate state
  cannot replace the existing state.
- Run the middleware `--self-test`; assert URL sanitization removes both token
  query data and `JSESSIONID` path data.
- Parse the PowerShell file and run fixture `--check-only`; assert no browser,
  file write, or process restart occurs.
- Occupy a temporary port with an unrelated listener; assert it remains alive
  after launcher rejection.
- Start a fixture server through the launcher and launch again; assert the same
  PID is reused.
- Complete one real visible-browser login, then assert storage state timestamp
  advances, the managed listener PID changes, `/health` is 200, and a real
  room/date query is 200 without cache.
- Send the real query with the firmware-configured token and assert 200; a 401
  means desktop and firmware token configuration is inconsistent.

### 7. Wrong vs Correct

Wrong:

```powershell
Stop-Process -Id (Get-NetTCPConnection -LocalPort 8080).OwningProcess -Force
```

Correct:

```powershell
$state = Get-ScheduleServerPortState -Port 8080 `
    -ServerScript $serverScript -LauncherScript $PSCommandPath
if ($state.Kind -eq "project_server") {
    Stop-VerifiedScheduleServer -Port 8080 -ExpectedProcessId $state.ProcessId `
        -ServerScript $serverScript -LauncherScript $PSCommandPath
}
```
