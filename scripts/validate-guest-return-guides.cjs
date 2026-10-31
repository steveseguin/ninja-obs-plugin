const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const { chromium } = require('playwright');
const root = path.resolve(__dirname,'../docs');
const output = path.resolve(process.argv[2] || 'artifacts/guest-return-guide-review');
fs.mkdirSync(output,{recursive:true});
const report = [];
const server = http.createServer((req,res)=>{
  const file = path.resolve(root,'.'+decodeURIComponent(new URL(req.url,'http://localhost').pathname));
  if(!file.startsWith(root+path.sep)||!fs.existsSync(file)||!fs.statSync(file).isFile()){res.writeHead(404).end();return}
  res.setHeader('Content-Type',({'.html':'text/html; charset=utf-8','.css':'text/css','.png':'image/png'})[path.extname(file)]||'application/octet-stream');
  fs.createReadStream(file).pipe(res);
});
(async()=>{
  await new Promise(r=>server.listen(0,'127.0.0.1',r));
  const browser=await chromium.launch({channel:'chrome'});
  try {
    for(const viewport of [{width:1280,height:900},{width:390,height:844}]) {
      const page=await browser.newPage({viewport});
      const errors=[];page.on('pageerror',e=>errors.push(e.message));
      for(const name of ['guest-return','guest-return-join','guest-return-rehearsal']) {
        await page.goto('http://127.0.0.1:'+server.address().port+'/'+name+'.html');
        await page.locator('details').evaluateAll(ds=>ds.forEach(d=>d.open=true));
        for(const img of await page.locator('img').all())await img.scrollIntoViewIfNeeded();
        await page.waitForFunction(()=>Array.from(document.images).every(img=>img.complete&&img.naturalWidth>0));
        assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'Horizontal overflow: '+name);
        const links=await page.locator('a[href]').evaluateAll(as=>as.map(a=>a.getAttribute('href')));
        for(const href of links.filter(h=>!/^https?:|^#/.test(h)))assert(fs.existsSync(path.resolve(root,href.split('#')[0])),'Missing guide link '+href);
        const prefix=name+'-'+viewport.width;
        await page.screenshot({path:path.join(output,prefix+'-full.png'),fullPage:true});
        const sections=await page.locator('.hero,.flow,.step,.checks').all();
        for(let i=0;i<sections.length;i++)await sections[i].screenshot({path:path.join(output,prefix+'-section-'+String(i+1).padStart(2,'0')+'.png')});
        if(name==='guest-return') {
          await page.locator('#viewers').fill('4');await page.locator('#bitrate').fill('1500');
          assert.match(await page.locator('#estimate').innerText(),/8\.5 Mbps/);
        }
        if(name==='guest-return-rehearsal') {
          const checks=await page.locator('input[type=checkbox]').all();assert.equal(checks.length,8);
          for(const check of checks)await check.check();
          assert.equal(await page.locator('input:checked').count(),8);
        }
        if(viewport.width===1280)await page.pdf({path:path.join(output,name+'.pdf'),format:'A4',printBackground:true,margin:{top:'12mm',bottom:'12mm',left:'12mm',right:'12mm'}});
        report.push({page:name,viewport,images:await page.locator('img').count(),links:links.length,overflow:false,errors:[...errors]});
      }
      await page.close();
    }
    assert(report.every(r=>!r.errors.length));
  }finally{await browser.close();server.close();fs.writeFileSync(path.join(output,'report.json'),JSON.stringify(report,null,2))}
  console.log('Three guides passed desktop/mobile layout, image/link, calculator and checklist checks. Screenshots and PDFs: '+output);
})().catch(e=>{console.error(e.message);server.close();process.exitCode=1});
