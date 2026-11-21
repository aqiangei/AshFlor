const {chromium} = require('playwright');
const assert = require('assert');

(async () => {
  const browser = await chromium.launch({headless: true});
  const page = await browser.newPage();
  try {
    await page.goto(process.env.GUIDE_URL || 'http://127.0.0.1/', {waitUntil: 'domcontentloaded'});
    await page.evaluate(() => {
      const dialog = document.getElementById('serverNotice');
      if (dialog && typeof dialog.showModal === 'function') dialog.showModal();
    });
    const official = page.locator('.server-official-site');
    await official.waitFor({state: 'attached'});
    assert.equal(await official.locator('strong').innerText(), '氪金联系QQ');
    const qq = official.locator('a');
    assert.equal(await qq.innerText(), '3028753757');
    assert.equal(
      await qq.getAttribute('href'),
      'https://wpa.qq.com/msgrd?v=3&uin=3028753757&site=qq&menu=yes');
    assert.equal(await qq.getAttribute('target'), '_blank');
    assert.equal(await page.locator('[data-afdian]').count(), 0);
    assert.equal(await page.locator('[data-server-renewal]').count(), 0);
    assert.deepEqual(
      await page.locator('[data-server-topup-tips] li').allInnerTexts(),
      [
        '25 元一个随机 Apex',
        '10 元一个永久超凡拟态（Mimic）',
        '10 元迁移服务器',
        '三个服务器已续租半年，共 2892 元',
      ]);
    console.log(JSON.stringify({ok: true}));
  } finally {
    await browser.close();
  }
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
