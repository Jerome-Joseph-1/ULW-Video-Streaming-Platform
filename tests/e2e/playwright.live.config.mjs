import { defineConfig } from '@playwright/test';

// The live packager's own run, apart from the VOD stack's (playwright.config.mjs matches
// *.spec.mjs only, so run.sh never starts this one).
export default defineConfig({
  testDir: '.',
  testMatch: 'live.e2e.mjs',
  workers: 1,
  timeout: 240_000,
  reporter: [['list']],
  use: {
    headless: true,
    launchOptions: { executablePath: process.env.ULW_E2E_CHROME },
  },
});
