# Control panel

A single-page app for the pgdemo API: a login screen, a **visualization panel** for every role (each `GET` and the SSE stream, read-only), and an **administration panel** for the `admin` role (each `POST` and `DELETE`). It is Next.js 16 exported as static files, styled with Tailwind CSS v4 and the [Catalyst](https://catalyst.tailwindui.com/) components, and served by `pgdemo-server` itself on the same origin as the API. The interface is in Spanish; the code is in English.

![Dispositivos, live](docs/screens/devices.png)

- [Getting started](#getting-started)
- [Architecture](#architecture)
- [Design](#design)
- [Screens](#screens)
- [Tests](#tests)

## Getting started

| Need | For |
| --- | --- |
| Node.js 22 and pnpm 10 (`corepack enable` picks the version in `package.json`) | everything below |
| Your copy of the Catalyst UI kit (Tailwind Plus licence) | `pnpm dev` and `pnpm build` |
| The API running | `pnpm dev` |

Catalyst is licensed, so its sources are **not** in this public repository. `pnpm dev` and `pnpm build` first run [`scripts/setup-catalyst.sh`](scripts/setup-catalyst.sh), which copies the kit's `typescript/*.tsx` into `src/components/catalyst/` (gitignored) and puts our [`catalyst-overrides/link.tsx`](catalyst-overrides/link.tsx) over the kit's version, so Catalyst links go through `next/link`. It reads the kit from `CATALYST_DIR`, or from `~/management-sensors/frontend/private/catalyst-ui-kit/typescript` by default.

```bash
cd frontend
pnpm install
CATALYST_DIR=/path/to/catalyst-ui-kit/typescript pnpm dev    # http://localhost:3000
```

`pnpm dev` proxies `/api/*` to `PGDEMO_API` (default `http://127.0.0.1:8080`), so the browser still sees one origin. The operational stack speaks HTTPS on that port, so point `PGDEMO_API` at `https://127.0.0.1:8080` and trust its certificate. Compression is off in development: gzip would buffer the SSE stream.

```bash
pnpm build      # next build, then scripts/export-webroot.sh
```

`next build` writes the static export to `out/`; `postbuild` ([`scripts/export-webroot.sh`](scripts/export-webroot.sh)) refuses an export with source maps or with the kit's module source text in it, copies it to `../webroot/` and writes `webroot/VERSION` (commit, `-dirty` if `frontend/` has changes, and build time). The operational image serves `webroot/` when it has an `index.html`:

```text
[server] panel: 107 files from /…/webroot (1829 KiB), commit=cb2cc4118ce1 built_at=2026-11-09T22:01:25Z
```

**Without Catalyst**, download the panel CI built instead; the script checks its sha256:

```bash
scripts/fetch-webroot.sh              # Release webroot-latest
scripts/fetch-webroot.sh --tag <tag>  # another Release
```

### What CI builds and publishes

The `frontend` job in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) checks out only `frontend/private/catalyst-ui-kit/typescript` of the private `jgmatu/management-sensors` with a read-only deploy key, then runs `pnpm install --frozen-lockfile`, `pnpm lint`, `pnpm test` and `pnpm build`, and uploads `webroot/` as the artifact `webroot` (30 days). The `e2e` job builds the Docker image with that artifact and runs the whole Robot suite on it. On a push to `main`, `publish-webroot` puts `webroot.tar.gz` and `webroot.tar.gz.sha256` in the Release `webroot-latest`, so only a panel that passed the suite is published.

To set up the key once:

1. `ssh-keygen -t ed25519 -N '' -C ci-catalyst -f catalyst_deploy_key`
2. In `jgmatu/management-sensors`, Settings → Deploy keys: add `catalyst_deploy_key.pub`, read-only.
3. In `jgmatu/postgresql`, Settings → Secrets → Actions: add `CATALYST_DEPLOY_KEY` with the contents of `catalyst_deploy_key`.

Without the secret (a pull request from a fork, for one) the `frontend` job builds nothing and says so, and `e2e` tests the panel from `scripts/fetch-webroot.sh`.

## Architecture

### Layers

```mermaid
flowchart TB
    subgraph app ["src/app (App Router, static export)"]
        login["page.tsx: login"]
        subgraph panel ["(panel)/layout.tsx: PanelShell"]
            view["devices, device, config, map, pki, events, system, history"]
            subgraph admin ["admin/layout.tsx: AdminShell"]
                adm["admin/devices, identity, config, users"]
            end
        end
        nf["not-found.tsx: 404.html"]
    end
    subgraph components ["src/components"]
        cat["catalyst/ (copied by setup-catalyst.sh)"]
        own["PanelShell, AdminShell, nav, badges, ui, charts (SVG), ConfirmDialog, Outcome"]
    end
    subgraph lib ["src/lib"]
        api["api.ts: fetch with Bearer, ApiError"]
        session["session.ts: sessionStorage"]
        sse["sse.ts: SSE parser over fetch"]
        hooks["hooks.ts: useApi, useStream, useSession"]
        types["types.ts: API shapes"]
    end
    app --> components
    app --> hooks
    own --> cat
    hooks --> api
    hooks --> sse
    api --> session
    sse --> session
```

Every page is a client component: the export is plain HTML and JavaScript, there is no Node server, and all data comes from the API after load. The `(panel)` route group puts the session check and the navbar around every page after login without adding a path segment; `admin/` nests a second layout inside it.

### Data flow

```mermaid
sequenceDiagram
    participant B as Browser (panel)
    participant S as pgdemo-server
    participant P as PostgreSQL
    B->>S: POST /api/auth/login {username, password}
    S-->>B: 200 {token, role, expires_in}
    B->>S: GET /api/auth/me (Authorization: Bearer)
    S-->>B: 200 {username, role, pg_role}
    B->>S: GET /api/devices (Bearer)
    S->>P: SET LOCAL ROLE demo_normal or demo_admin, SELECT
    S-->>B: 200 JSON
    B->>S: GET /api/stream?channels=telemetry_scalar,device (Bearer, fetch)
    S-->>B: 200 text/event-stream: retry: 3000, then id/event/data
    P-->>S: NOTIFY telemetry_scalar
    S-->>B: event: telemetry_scalar
    Note over B,S: dropped stream: wait retry ms and reconnect<br/>401: end the session, back to /?expired=1
```

- **REST** ([`lib/api.ts`](src/lib/api.ts)) sends `Authorization: Bearer <token>` on every call and turns error bodies (`{"error","message"}`) into an `ApiError` with a Spanish description. A 401 on anything but the login ends the session.
- **SSE** ([`lib/sse.ts`](src/lib/sse.ts)): `EventSource` cannot send headers and the token must not go in a URL (it would end up in logs and history), so the stream is read with `fetch()` and a `ReadableStream` and parsed by `SseParser`. It honours `retry:`, reconnects after a drop, stops on 401 (session over) and on 400 (bad filter). Filters go to the server as `?channels=&devices=`, so a page only receives what it shows.
- **Session** ([`lib/session.ts`](src/lib/session.ts)): token, user, role, PostgreSQL group and expiry in `sessionStorage` (per tab, gone with it), read through `useSyncExternalStore`. A timer ends it when the token expires.

### Role and visibility

```mermaid
flowchart LR
    login["POST /api/auth/login"] --> role["role in the session"]
    role --> me["GET /api/auth/me on load: role and pg_role confirmed by the server"]
    me --> shell{"PanelShell: admin?"}
    shell -- yes --> navA["navbar: visualization tabs + amber Administración + Gestionar buttons"]
    shell -- no --> navN["navbar: visualization tabs only"]
    navN -. "/admin/* typed by hand" .-> gate["AdminShell: back to /devices/"]
    navA --> server["every request: the server checks the role again (403)"]
    navN --> server
```

Hiding the administration entry is presentation, not security: the pages are public static files, and what protects the fleet is the server answering 403 to a `normal` token on every write (`tests/e2e/10__panel.robot` checks exactly that).

### From build to browser

```mermaid
flowchart LR
    src["frontend/ + Catalyst"] -->|"pnpm build"| out["out/"]
    out -->|"export-webroot.sh: no .map, no kit source"| web["webroot/ + VERSION"]
    ci["CI artifact webroot"] --> web
    rel["Release webroot-latest"] -->|"scripts/fetch-webroot.sh (sha256)"| web
    web -->|"--webroot"| site["StaticSite (in memory)"]
    site --> ctl["PanelController GET *"]
    ctl -->|"same origin as /api"| browser["Browser"]
```

`pgdemo-server --webroot webroot` loads every file into memory at start ([`server/src/http/StaticSite.hpp`](../server/src/http/StaticSite.hpp)) and registers `GET *` last, so it only receives what no API endpoint matched ([`PanelController.hpp`](../server/src/controllers/PanelController.hpp)). Anything under `/api` that reaches it is a JSON 404, a missing page is `404.html` with status 404, `/_next/static/*` (hashed names) is cached `immutable` and the rest `no-cache`, and every file carries `nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer` and a CSP limited to `'self'`. The token interceptor only guards `/api`. Same origin means no CORS and no API URL baked into the build.

## Design

### Two panels

The **visualization panel** is the default and the whole app for a `normal` user: seven tabs in a Catalyst `StackedLayout` navbar, nothing that writes. The **administration panel** appears only for `admin`, after a divider in the same navbar: an amber `Administración` entry with an `admin` badge, an amber banner on every admin page ("Estas acciones modifican la flota…"), its own sub-tabs, amber primary buttons and red ones for anything destructive, which always asks first (`ConfirmDialog`). On visualization pages the admin also gets amber **Gestionar** buttons that jump to the matching admin page. The user menu shows the user, a role badge (amber for admin, grey for normal) and the PostgreSQL group the server uses for that role.

### Routes

| Tab | Route | Role | API | SSE channels |
| --- | --- | --- | --- | --- |
| Login | `/` (`/?expired=1` after a 401) | anyone | `POST /api/auth/login`, `GET /api/auth/me` | — |
| Dispositivos | `/devices/` | all | `GET /api/devices` | `telemetry_scalar`, `telemetry_gps`, `telemetry_xy`, `telemetry_xyz`, `device` |
| Detalle | `/device/?name=<device>` | all | `GET /api/devices/{name}`, `…/history?limit=`, `…/certificates`, `GET /api/enrollments?device=`, `GET /api/config/history?device=` | every channel, `devices=<name>` |
| Configuración | `/config/` | all | `GET /api/config`, `GET /api/config/history`, `GET /api/config/requests/{id}` | `config`, `error` |
| Mapa | `/map/` | all | `GET /api/devices`, `GET /api/devices/{name}/history` | `telemetry_gps` |
| PKI | `/pki/` | all | `GET /api/pki/ca`, `GET /api/pki/origin`, `GET /api/pki/crl`, `GET /api/enrollments` | `enrollment`, `device` |
| Eventos | `/events/` | all | — | the ones ticked, `devices=` as typed |
| Sistema | `/system/` | all | `GET /api/health`, `GET /api/db/info` (every 5 s), `GET /api/device-types`, `GET /VERSION` | — |
| Histórico | `/history/` | all | `GET /api/devices`, `GET /api/devices/{name}/history?from=&to=&limit=`, `GET /api/config/history?device=`, `GET /api/enrollments?device=`, `GET /api/pki/ca` | — |
| Admin · Dispositivos | `/admin/devices/` | admin | `POST /api/devices` (then `GET /api/enrollments/{id}` until resolved), `DELETE /api/devices/{name}` | `device` |
| Admin · Identidad | `/admin/identity/?name=<device>` | admin | `POST`/`DELETE /api/devices/{name}/revoke`, `POST …/rotate`, `DELETE …/certificates/{serial}` | `device`, `enrollment`, `devices=<name>` |
| Admin · Configuración | `/admin/config/?device=<device>` | admin | `POST /api/config` (then `GET /api/config/requests/{id}` after a 202) | `config`, `error` |
| Admin · Usuarios | `/admin/users/` | admin | `GET`/`POST /api/users`, `DELETE /api/users/{username}` | — |

Routes end in `/` because the export writes `devices/index.html`; the server also answers `/devices`. Query strings instead of dynamic segments (`/device/?name=`) keep a fixed set of exported pages for any device name.

### Components

Catalyst provides the primitives: layout, navbar, sidebar (mobile), dropdown, table, dialog, fieldset, input, select, switch, checkbox, badge, button, description list, auth layout. Our own components sit on top: `PanelShell` and `AdminShell` (session, role, navigation), `badges.tsx` (one badge per API state set, Spanish label, raw value in `data-state`), `ui.tsx` (headings, callouts, stats, the live indicator), `ConfirmDialog` and `Outcome`. The scalar, GPS and `xy` charts are plain SVG in `charts.tsx`: a line chart with round-number ticks, sparklines, a square scatter cloud, and a map in metres around the device origin (40.4168, −3.7038) with the track and the current fix. An `xyz` device uses [`point-cloud.tsx`](src/components/point-cloud.tsx): React Three Fiber draws each sample as one instanced sphere, the same blue, lit inside the canvas. `@react-three/drei` `OrbitControls` moves the camera on a sphere around the origin. The readout is the radius `r`, the azimuth `θ` around Y and the polar angle `φ` from +Y. The libraries are bundled; the CSP stays `'self'`. No map tiles, no web fonts. The trust circle on PKI uses Motion (`motion/react`) so each entity's certificate — factory, manager and every worker — fades in with its fields.

### `data-testid`

Every element a test needs has a `data-testid` (`nav-devices`, `nav-admin`, `device-row-<name>`, `badge-identity`, `dialog-enroll`, `identity-result`…). States the tests check are in `data-state` with the API value (`ONLINE`, `SUSPENDED`, `applied`, `last-admin`), so the tests depend neither on the Spanish text nor on Tailwind classes. Admin actions report their result in an `Outcome` callout whose `data-state` names what happened (`issued`, `conflict`, `rejected`, `rotated`, `deleted`…).

## Screens

Captured at 1440x900 (see [Tests](#tests)). The UI suites produce them against the Docker stack. The PKI capture is the whole page, so the trust-circle cards stay in frame above the demo CA. The 3D cloud is the exception: `cloud` is enrolled by an admin, so that capture comes from a browser check.

### Login

![Login](docs/screens/login.png)

`/`: Catalyst `AuthLayout`. `POST /api/auth/login`, then `GET /api/auth/me` for the PostgreSQL group.

![Wrong password](docs/screens/login-error.png)

A 401 from the login shows "Usuario o contraseña incorrectos" and stays on the page.

![Session expired](docs/screens/login-expired.png)

Any other 401 (expired or forged token) ends the session and comes back here with `?expired=1`.

### Visualization (user `alice`, role `normal`)

![Dispositivos](docs/screens/devices.png)

Dispositivos: `GET /api/devices` once, then the values, `seq`, sparklines and counters move with `telemetry_scalar`, `telemetry_gps`, `telemetry_xy`, `telemetry_xyz` and `device` events; a `device` event (enrollment, removal, session change) reloads the list. A scalar device, including `white_noise`, keeps the sparkline. A scatter device shows `x, y` and a cloud device shows `x, y, z`, both instead of the sparkline.

![Detalle](docs/screens/device-detail.png)

Detalle de `dummy1`: the history window (`GET /api/devices/{name}/history`) plotted and extended live, the applied and desired configuration, the certificates, enrollments and the latest stored samples with their partition. A scalar device, including `white_noise`, keeps that line chart. The amplitude of `white_noise` is the standard deviation of the normal draw.

![Nube de puntos](docs/screens/device-scatter.png)

Detalle de un dispositivo `xy`: the same window drawn as a square cloud (`ScatterChart`). The axes stay equal and cover at least `[-amplitude, amplitude]` (100 by default); a point outside that square widens both axes together. Points come from the history and then from `telemetry_xy`. The table lists `x, y` and the partition `telemetry_xy_yYYYYmMM`.

![Nube 3D](docs/screens/device-cloud.png)

Detalle de un dispositivo `xyz` (`cloud`): the same window, each sample an instanced sphere. Dragging orbits the camera and the wheel changes its distance. `r`, `θ` and `φ` next to the canvas are that position, in degrees for the two angles. The scene axes are the database axes, with Y up. Points arrive on `telemetry_xyz`. The table lists `x, y, z` and the partition `telemetry_xyz_yYYYYmMM`. The line chart, the GPS map and the square cloud are unchanged.

![Configuración](docs/screens/config.png)

Configuración: `v_device_config_sync_status` through `GET /api/config`, the applied history, and a lookup of one request by id.

![Mapa](docs/screens/map.png)

Mapa: the last 120 fixes of each GPS device (`/history`, then `telemetry_gps`), in metres around the origin.

![PKI](docs/screens/pki.png)

PKI: the trust circle first (`GET /api/pki/origin`). The factory root, the manager certificate and each worker certificate are cards; Motion reveals the subject, serial, fingerprint and validity, and a new certificate replaces its card. Then the list the manager signs. Below that, the demo CA and the fingerprint it signs with (`GET /api/pki/ca`), its CRL (`GET /api/pki/crl`, shown or downloaded as PEM), and the enrollment requests. The `device` and `enrollment` stream reloads the origin. This capture is the end-to-end stack, which has no factory, so the three entities are empty and the demo CA is what the page shows underneath.

![Eventos](docs/screens/events.png)

Eventos: the raw stream; channels and devices are filtered by the server (`?channels=&devices=`). `telemetry_xy` is labelled Dispersión 2D and `telemetry_xyz` is labelled Nube 3D. It can be paused and emptied.

![Sistema](docs/screens/system.png)

Sistema: `GET /api/health`, `GET /api/db/info` (PostgreSQL, connection pool, SSE subscribers, listener), the device types, the session and the panel version (`/VERSION`).

![Histórico](docs/screens/history.png)

Histórico: a read-only audit under the session role (`demo_normal` or `demo_admin`). Samples of one device (`GET /api/devices/{name}/history`, last hour when no dates are set, with the partition), applied configuration (`GET /api/config/history`), enrollments and rotations (`GET /api/enrollments`, including who asked), and revocations from `GET /api/pki/ca`. A scalar sample shows its value, a GPS sample its coordinates, an `xy` sample `x, y`, and an `xyz` sample `x, y, z`.

![Dispositivos, usuario normal](docs/screens/devices-normal.png)

`alice` on Dispositivos: the visualization tabs only. There is no Administración entry and no Gestionar button.

![Normal user menu](docs/screens/user-menu-normal.png)

The same navbar for `alice`: no Administración entry and no Gestionar buttons; the menu shows `normal` and `demo_normal`.

### Administration (user `admin`)

![Admin user menu](docs/screens/user-menu-admin.png)

For the admin the navbar adds the amber Administración entry, the menu shows `admin` and `demo_admin`, and Dispositivos has a Gestionar button.

![Admin · Dispositivos](docs/screens/admin-devices.png)

Administración · Dispositivos: the amber banner and sub-tabs, the fleet with links to each identity and a delete action.

![Enrollment dialog](docs/screens/admin-enroll-dialog.png)

Dar de alta: name and type; `POST /api/devices`, and on a 202 the panel follows `GET /api/enrollments/{id}` until the certificate is issued or rejected.

![Enrolled](docs/screens/admin-enrolled.png)

The outcome: `ui-a` issued, `ACTIVE` and `ONLINE`.

![Admin · Configuración](docs/screens/admin-config.png)

Administración · Configuración: `POST /api/config` applied (200), with the applied values updated by the `config` event.

![Rejected configuration](docs/screens/admin-config-rejected.png)

`rate_ms` 5: the controller refuses it and the 422 carries `INVALID_RATE: rate_ms 5 outside [50, 10000]`; the applied configuration does not change.

![certificateHold](docs/screens/admin-identity-suspended.png)

Administración · Identidad: `certificateHold` suspends `ui-a` (`POST /api/devices/{name}/revoke`); Levantar suspensión (`DELETE …/revoke`) brings it back.

![Rotated](docs/screens/admin-identity-rotated.png)

Rotar (`POST …/rotate`): a new serial; the old certificate stays in the list as `superseded` and can be deleted.

![Permanent revocation](docs/screens/admin-revoke-dialog.png)

Any reason other than `certificateHold` is permanent, so it asks first.

![Admin · Usuarios](docs/screens/admin-users.png)

Administración · Usuarios: `GET`/`POST /api/users`; a new `normal` user.

![Last admin](docs/screens/admin-users-last-admin.png)

Deleting the last admin: PostgreSQL's trigger refuses it and the 422 is shown as such.

## Tests

```bash
pnpm test     # vitest: lib/ (api, session, sse parser and reconnection, poll, format)
pnpm lint     # eslint with the Next.js and React hooks rules
```

The interface is tested end to end by Robot Framework Browser against the Docker stack, after the API suites: [`tests/e2e/11__ui_login_and_roles.robot`](../tests/e2e/11__ui_login_and_roles.robot), [`12__ui_visualization.robot`](../tests/e2e/12__ui_visualization.robot) and [`13__ui_admin.robot`](../tests/e2e/13__ui_admin.robot), with the keywords in [`resources/ui.resource`](../tests/e2e/resources/ui.resource); [`10__panel.robot`](../tests/e2e/10__panel.robot) checks the static serving over plain HTTP. See [tests/e2e/README.md](../tests/e2e/README.md). The screens above come from those suites; to regenerate them:

```bash
.venv/bin/robot --outputdir build/e2e --variable UI_SCREENSHOTS:frontend/docs/screens tests/e2e
```
