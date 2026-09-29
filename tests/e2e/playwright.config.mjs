import { defineConfig } from '@playwright/test';

// run.sh sets ULW_E2E_CHROME: Playwright's own Chromium is built without H.264 and AAC, which
// is all the worker writes, so the suite drives a Chrome build of the same version instead.
export default defineConfig({
  testDir: '.',
  testMatch: '*.spec.mjs',
  // One stack per run: the tests share the local Postgres and MinIO.
  workers: 1,
  timeout: 300_000,
  reporter: [['list']],
  use: {
    headless: true,
    launchOptions: { executablePath: process.env.ULW_E2E_CHROME },
  },
});
