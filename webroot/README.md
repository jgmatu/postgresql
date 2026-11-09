# webroot/

The control panel that `pgdemo-server --webroot webroot` serves on the same
origin as the API. Only this README is in git: the files are a build output,
and the Catalyst components they come from are licensed and stay out of this
public repository.

Fill it in one of two ways:

| With | Command | Result |
| --- | --- | --- |
| No Catalyst copy | `scripts/fetch-webroot.sh` | downloads `webroot.tar.gz` from the Release [`webroot-latest`](https://github.com/jgmatu/postgresql/releases/tag/webroot-latest), checks its sha256 and extracts it here |
| A Catalyst copy | `cd frontend && pnpm install && pnpm build` | `next build` exports to `frontend/out/`, and `postbuild` checks it (no source maps, no Catalyst source text) and copies it here |

`VERSION` records the commit the panel was built from and when. The server
logs it at start (`panel: N files from …`) and the Sistema tab shows it.

`scripts/run-all.sh` (and the Docker image, which copies this directory)
passes `--webroot` only when `index.html` is here; otherwise the server runs
the API alone and says "panel no disponible". See
[frontend/README.md](../frontend/README.md) for the panel itself.
