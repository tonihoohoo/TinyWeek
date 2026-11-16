// Regenerates the README images from shots.html.
// Usage: npm i playwright && npx playwright install chromium && node docs/screenshots/shoot.js
const {chromium} = require('playwright');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({viewport: {width: 960, height: 2000}, deviceScaleFactor: 2});
  await p.goto('file://' + __dirname + '/shots.html');
  await p.waitForSelector('body[data-ready]');
  for (const id of ['hero', 'menu', 'themes'])
    await p.locator('#' + id).screenshot({path: `${__dirname}/${id}.png`, omitBackground: true});
  await b.close();
})();
