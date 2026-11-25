const assert = require('node:assert/strict'), fs = require('node:fs'), vm = require('node:vm');
const {combineRanking} = require('./unified-council.cjs');
const player = (id,apexCount)=>({id,name:id,apexCount,level:10,levelScore:9,recordedSeconds:3600});
const rows = combineRanking([{realm:1,players:[player('a',2)]},{realm:2,players:[player('b',1)]},{realm:3,players:[player('c',0)]}]);
assert.deepEqual(rows.map(r=>r.units),[1300000,1000000,700000]);
const empty=combineRanking([{realm:1,players:[player('a',0),player('b',0)]}]);assert.equal(empty[0].units,1500000);assert.ok(Math.abs(empty[0].activityWeight-5/7)<1e-12);
for(const [file,gateway] of [['./council.html',true],['../../../cpp/client/web/council.html',false]]){
 const html=fs.readFileSync(require('node:path').resolve(__dirname,file),'utf8');const js=html.match(/<script>([\s\S]*?)<\/script>/)[1];new vm.Script(js);
 const node=(tag,text)=>({tag,text,dataset:{},children:[],append(...nodes){this.children.push(...nodes)},replaceChildren(){this.children=[]}});
 const elements={};const $=id=>elements[id]??=(Object.assign(node(id),{value:''}));
 const ctx={node,$,ranking:rows,currentAccount:'',duration:String,realmName:String,pct:String,rankingTarget:0};
 const fn=js.match(/function renderRanking\([^\n]*\n[\s\S]*?\n}/)[0];vm.runInNewContext(fn,ctx);ctx.renderRanking({players:rows,pagination:{page:1,pages:1,matches:3,query:''}});
 assert.equal($('players').children.length,3);assert.deepEqual($('players').children.map(tr=>tr.children[5].text),[2,1,0]);assert.ok($('players').children.every(tr=>tr.children.length===7&&tr.children.every(c=>c.tag==='td')));assert.doesNotMatch(js,/expandPlayer/);
}
console.log('Gateway allocation and both ranking pages passed');
