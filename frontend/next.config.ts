import type { NextConfig } from "next";

/**
 * `next build` produces a static export (`out/`, copied to ../webroot by
 * scripts/export-webroot.sh) that pgdemo-server serves on the same origin as
 * the API. `next dev` instead proxies /api to a running server, so the
 * browser stays same-origin there too and the server needs no CORS.
 */
const api = (process.env.PGDEMO_API ?? "http://127.0.0.1:8080").replace(/\/+$/, "");

const common: NextConfig = {
  trailingSlash: true,
  // Next would otherwise write an AGENTS.md into the project on every start.
  agentRules: false,
};

const production: NextConfig = {
  ...common,
  output: "export",
  images: { unoptimized: true },
  // A source map would carry the Catalyst sources into the public artifact.
  productionBrowserSourceMaps: false,
};

const development: NextConfig = {
  ...common,
  // Otherwise /api/devices is redirected to /api/devices/ before the rewrite.
  skipTrailingSlashRedirect: true,
  // gzip would buffer the proxied /api/stream and hold back every event.
  compress: false,
  async rewrites() {
    return [{ source: "/api/:path*", destination: `${api}/api/:path*` }];
  },
};

export default process.env.NODE_ENV === "production" ? production : development;
