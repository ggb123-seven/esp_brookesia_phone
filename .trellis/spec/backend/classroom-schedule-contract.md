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
