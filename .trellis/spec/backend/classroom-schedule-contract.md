# Classroom Schedule Contract

## 1. Scope / Trigger

Use this contract when changing the classroom schedule firmware client, the
Windows middleware, EAMS session handling, building mappings, or schedule
fixtures. These pieces form one cross-layer request path and must be validated
together.

## 2. Signatures

- Device API: `GET /classroom-schedule/today`
- Query: `classroom=<UTF-8 name>&date=YYYY-MM-DD&token=<secret>`
- Health API: `GET /health`
- Provider entry point:
  `EamsRoomOccupancyProvider.fetch_schedule(classroom, date_text)`

## 3. Contracts

A successful response is HTTP 200 JSON with matching `classroom` and `date`, a
display `classroom_name`, an `updated_at` string, and a `courses` array. Each
course contains `start`, `end`, `name`, `teacher`, and `group` strings.

The firmware must reject a response whose `classroom` or `date` differs from
the active query. Offline cache reuse also requires the same server host,
classroom, and date.

EAMS building routing has three independent values: the firmware display name,
`room.building.id`, and the upstream `buildingname` query value. Never derive
the last two from the display name. For example, `知行楼` uses `buildingname=育龙主楼`,
while `博文楼`, `中和楼`, and `致远楼` use the literal value `null`.

The local session file may reference a Playwright storage-state file. Cookie
values and API tokens must stay in ignored local files and must never be logged.

## 4. Validation & Error Matrix

| Condition | Middleware result | Firmware behavior |
| --- | --- | --- |
| Missing/invalid token | HTTP 401 | Show token configuration error |
| Invalid classroom/date | HTTP 400 | Show request/configuration error |
| EAMS 3xx login redirect, 401, or 403 | HTTP 503 | Ask for EAMS re-login |
| Other upstream failure | HTTP 502 | Report upstream service unavailable |
| Response classroom/date mismatch | HTTP 200 rejected locally | Do not display or cache |
| Valid empty `courses` | HTTP 200 | Show explicit no-course state |

EAMS login redirects must terminate the syllabus scan immediately. Continuing
through all pages while holding the provider lock can exceed the device HTTP
timeout and make unrelated requests wait behind the expired session.

## 5. Good / Base / Bad Cases

- Good: `尔雅楼103 / 2026-06-04` returns HTTP 200 with the real three-entry
  schedule and matching query fields.
- Base: a valid room/date with no courses returns HTTP 200 and `courses: []`.
- Bad: an expired storage state redirects to login; middleware returns HTTP 503
  promptly without exposing redirect URLs, cookies, or tokens.

## 6. Tests Required

- Run `real_classroom_schedule_server.py --self-test` and assert 200/400/401/404
  behavior plus fixture date propagation.
- Unit-check every built-in building display name, id, and upstream query name;
  also assert local config overrides win.
- Query `/health`, then one real EAMS room/date; assert HTTP 200, matching fields,
  and a bounded response time below the firmware timeout.
- Run the firmware font coverage check, `idf.py build`, flash COM3, and verify
  schedule refresh on hardware.

## 7. Wrong vs Correct

Wrong:

```python
query["buildingname"] = displayed_building_name
```

Correct:

```python
building_name = provider._building_name_for(classroom_name)
query["room.building.id"] = provider._building_id_for(classroom_name)
query["buildingname"] = provider._building_query_name_for(classroom_name)
```

## 8. Windows Session Refresh Launcher

### 1. Scope / Trigger

Use this contract when changing the Windows launcher, EAMS browser login,
Playwright storage state, local schedule token wiring, or managed server
restart behavior. The user-facing workflow must remain a single desktop batch
entry even when internal helpers are separate modules.

### 2. Signatures

- Desktop entry: `start_classroom_schedule_server.bat [--check-only|--autostart]`
- PowerShell orchestrator:
  `tools/windows/start_classroom_schedule_server.ps1 [-CheckOnly] [-AutoStart] [-SkipSessionCheck]`
- Browser helper:
  `python tools/eams_session_refresh.py --session-file <path> --login-file <path> --profile-dir <path> --timeout-seconds <30..3600>`
- Refresh settings: `EAMS_BROWSER_PROFILE`, `EAMS_LOGIN_TIMEOUT_SECONDS`,
  `EDGE_EXECUTABLE`, and `REFRESH_EAMS_SESSION`.
- The desktop `SCHEDULE_API_TOKEN` must equal firmware
  `CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_TOKEN`; compare values without logging
  either value.

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
