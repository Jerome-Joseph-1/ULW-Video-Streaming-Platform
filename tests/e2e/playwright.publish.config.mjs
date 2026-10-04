import { defineConfig } from '@playwright/test';

// Live publishing through the stream service (live-publish.e2e.mjs, and
// live-publish-webhook.e2e.mjs where LiveKit's webhooks do the starting and ending; run by
// live-publish-run.sh): Chromium's fake camera and microphone, its host candidates as addresses
// rather than mDNS names (LiveKit resolves none), and the same pinned Chrome for Testing as the
// other runs, whose H.264 and AAC decoders the recorder's output needs.
export default defineConfig({
  testDir: '.',
  testMatch: 'live-publish*.e2e.mjs',
  workers: 1,
  timeout: 420_000,
  reporter: [['list']],
  use: {
    headless: true,
    launchOptions: {
      executablePath: process.env.ULW_E2E_CHROME,
      args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
        '--disable-features=WebRtcHideLocalIpsWithMdns', '--autoplay-policy=no-user-gesture-required'],
    },
  },
});
