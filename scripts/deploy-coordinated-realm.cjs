'use strict';
// Run on the owner's Windows computer. A stopped snapshot must reach this
// computer and pass SHA256 verification before any live files are replaced.
// Usage: node scripts/deploy-with-backup.cjs RELEASE_DIRECTORY [BACKUP_ROOT]
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const cp=require('node:child_process'),assert=require('node:assert/strict');
const clientAssets=require('./shards/ops/client-assets.cjs');
const release=process.argv[2];
if(!/^\/opt\/florrclone\/releases\/[a-zA-Z0-9_-]+$/.test(release||''))
  throw Error('Supply a staged /opt/florrclone/releases/RELEASE directory');
const root=path.resolve(__dirname,'..');
const backupRoot=path.resolve(process.argv[3]||path.join(root,'../backups/florrclone'));
const stamp=new Date().toISOString().replace(/[-:]/g,'').replace(/\.\d+Z$/,'Z');
const local=path.join(backupRoot,stamp);
const archiveTool=path.join(__dirname,'transfer-archive-backup.py');
cp.execFileSync('python',[archiveTool,'init',local],{stdio:'ignore'});
const target=process.env.FLORR_DEPLOY_TARGET_FILE?JSON.parse(fs.readFileSync(process.env.FLORR_DEPLOY_TARGET_FILE,'utf8')):{};
assert(!target.host||/^[a-zA-Z0-9.-]+$/.test(target.host));
const key=target.key||path.join(process.env.USERPROFILE||process.env.HOME,'.ssh/ASHFLOR.pem');
assert([undefined, 'fresh', 'third'].includes(target.instance), 'Unknown deployment instance');
const fresh = target.instance === 'fresh';
const third = target.instance === 'third';
const role = fresh ? 'second' : third ? 'third' : 'first';
const coordinated=process.env.FLORR_COORDINATED_RUN;
assert(coordinated,'A coordinated run is required');
const barrier=require('./coordinated-barrier.cjs')(coordinated,role);
const publicEntry='http://'+target.host+':67/';
assert.equal(target.host, ({first:'202.189.8.109', second:'202.189.23.253', third:'103.91.210.16'})[role]);
const transport = process.env.ASHFLOR_SERIAL_DEPLOY_TRANSPORT;
assert(transport, 'Set ASHFLOR_SERIAL_DEPLOY_TRANSPORT to the operator transport');
const host='root@'+(target.host||'154.219.105.161');
const service = third ? 'florr-third' : fresh ? 'florr-fresh' : 'florrclone';
const installOwner = third ? 'florr-third' : 'florrclone';
const runtime = third ? '/usr/bin/node --experimental-websocket' : '/opt/florrclone/runtime/bin/node';
const current = third ? '/opt/florrclone-third/current' : '/opt/'+service+'/current';
const stateRoot = '/var/lib/'+service;
const databaseFile = stateRoot + '/cpp-state/inventory.json';
const databaseNext = stateRoot + '/cpp-state/inventory.next.json';
const remote = stateRoot + '/cutover-backups/' + stamp;
const remoteArchiveTool = remote+'/transfer-archive-backup.py';
const archiveFiles = ['transfer-archives.tar.gz','transfer-archives-manifest.json'];
let archiveProof;
const maintenance = third ? stateRoot+'/maintenance' : fresh ? '/var/www/ashflor-direct-maintenance' : '/var/www/florrclone-maintenance';
const marker = maintenance + '/enabled';
const nginxConfig = fresh ? '/etc/nginx/sites-enabled/ashflor-preflight' : '/etc/nginx/conf.d/'+(third ? 'florr-third' : 'florrclone')+'.conf';
const publicPort = fresh ? 81 : 80;
const backendPort = third ? 38788 : fresh ? 8789 : 8788;
const backend = 'http://127.0.0.1:' + backendPort;
const run=(exe,args,input)=>{
  for(let attempt=0;;attempt++)try{return cp.execFileSync(exe,args,{input,encoding:'utf8',maxBuffer:1024*1024});}
  catch(error){
    // Retry only failures before SSH authentication/command execution. Never
    // replay a command after an uncertain remote execution or a command error.
    const preConnect=/^(?:ssh: connect to host .*: (?:Connection timed out|No route to host)|Connection timed out during banner exchange)\r?$/m.test(String(error.stderr||''));
    const sshTransport = exe === 'python' && args[0] === transport;
    if(!(['ssh','scp'].includes(exe)||sshTransport)||!preConnect||attempt>=3)throw error;
  }
};
const sshOptions=['-i',key,'-o','BatchMode=yes','-o','ConnectTimeout=15'];
if(process.env.FLORR_DEPLOY_SSH_PROXY)sshOptions.push('-o','ProxyCommand='+process.env.FLORR_DEPLOY_SSH_PROXY);
const ssh=(s,input)=>run('python',[transport,role,'run',s],input);
const scp=(a,b)=>a.startsWith(host+':')
  ? run('python',[transport,role,'download',a.slice(host.length+1),b])
  : run('python',[transport,role,'upload',a,b.slice(host.length+1)]);
const q=s=>"'"+s.replace(/'/g,"'\\''")+"'";
let gated=false,locked=false,stopped=false,saveInstallStarted=false,switchAttempted=false,mayHaveOpened=false,verifiedBackupHash='',old='',bonusStoppedAt=0,bonusGatedAt=0,bonusConfirmedStoppedAt=0,bonusHelper='';
let freezeDropBonus=false;
function preserveMaintenanceBonus(){
  assert(bonusHelper&&bonusStoppedAt>0,'Maintenance bonus preparation missing');
  const state=ssh('systemctl show '+service+' -p MainPID -p ActiveState -p SubState');
  assert(state.includes('MainPID=0')&&state.includes('ActiveState=inactive')&&state.includes('SubState=dead'),'Bonus adjustment requires a stopped game');
  // A maintenance page only blocks new HTTP/WebSocket handshakes. The old
  // process can still serve upgraded sessions until graceful shutdown.
  const boundary=freezeDropBonus?bonusConfirmedStoppedAt:bonusStoppedAt;
  assert(Number.isSafeInteger(boundary)&&boundary>0,'Missing maintenance boundary');
  const result=JSON.parse(ssh('test -f '+q(marker)+' && '+runtime+' '+q(bonusHelper)+' '+q(databaseFile)+' '+q(stamp)+' '+boundary));
  const receipt={...result,gateClosedAt:bonusGatedAt,stopRequestedAt:bonusStoppedAt,
    stopConfirmedAt:bonusConfirmedStoppedAt};
  if(freezeDropBonus){
    delete receipt.gatedAt;
    receipt.freezeCutoffAt=boundary;
  }
  fs.writeFileSync(path.join(local,'maintenance-bonus.json'),JSON.stringify(receipt,null,2));
  return receipt;
}
function nginxFor(version, downloadQueue = process.env.FLORR_DOWNLOAD_QUEUE === '1', assetHashes, assetMirror) {
  let base = fs.readFileSync(path.join(__dirname, 'shards/ops/nginx-game.conf'), 'utf8');
  if (fresh || third) {
    base = base.replaceAll('8088 default_server', '10000')
      .replaceAll('127.0.0.1:8788', '127.0.0.1:' + backendPort)
      .replaceAll('florrclone_connection_upgrade', service.replaceAll('-', '_')+'_connection_upgrade');
  }
  return clientAssets.configure(base, {
    release: version,
    assetHost: process.env.FLORR_CLIENT_ASSET_HOST,
    assetPort: publicPort,
    downloadQueue,
    entryProxy: process.env.FLORR_ENTRY_PROXY_IP,
    assetHashes,
    assetMirror,
  }).replaceAll('/var/www/florrclone-maintenance/', maintenance + '/');
}
function installNginx(localFile, backupFile) {
  const staging = remote + '/nginx-next.conf';
  scp(localFile, host + ':' + staging);
  ssh('cp '+q(nginxConfig)+' '+q(backupFile)+' && cp '+q(staging)+' '+q(nginxConfig)+
      ' && { nginx -t && systemctl reload nginx || { cp '+q(backupFile)+' '+q(nginxConfig)+
      '; nginx -t && systemctl reload nginx; exit 1; }; }');
}
const {renderLiveFrontend}=require('./live-frontend.cjs');
async function main(){
  const announcements=require("./deploy-coordinated-announcement.cjs");
  const noticeConfig=announcements.readConfig(process.env.FLORR_DEPLOY_ADMIN_FILE);
  const verifyHello=(port, protocolVersion, digest) => {
    const code = `const w=new WebSocket('ws://127.0.0.1:${port}/ws','binary');
    w.binaryType='arraybuffer';setTimeout(()=>process.exit(2),10000).unref();
    w.onopen=()=>{const b=Buffer.alloc(11);b.writeUInt32LE(7);b[4]=1;b.writeUInt16LE(${protocolVersion},5);b.writeUInt32LE(${digest},7);w.send(b)};
    w.onmessage=e=>{const b=Buffer.from(e.data);w.close();process.exit(b[4]===1&&b[7]===1?0:1)};`;
    ssh(runtime+' -e '+q(code));
  };
  const bootstrap=process.env.FLORR_BOOTSTRAP_DEPLOY_ADMIN==='1';
  try{
    const resetFile=process.env.FLORR_PASSWORD_RESET_FILE;
    const preserveSave=process.env.FLORR_PRESERVE_SAVE==='1'||Boolean(resetFile);
    const resetRequests=resetFile?require('./reset-password-offline.cjs').readRequests(resetFile):null;
    const councilManagerId=process.env.FLORR_FRESH_COUNCIL_MANAGER_ID;
    if(councilManagerId)assert(fresh && preserveSave && !bootstrap,
      'Council delegation requires the independent second-server save');
    if(resetRequests) {
      assert(!fresh&&!third&&!bootstrap&&!process.env.FLORR_RESTORE_SNAPSHOT &&
        !process.env.FLORR_OPERATOR_HYPER_USER && process.env.FLORR_REFUND_ETERNAL_PRICE!=='1',
        'Password resets require an independent first-server update using its current save');
    }
    // Once shared authority is installed, the one-host path may neither replace
    // its local cache nor silently roll one server back to independent storage.
    assert(ssh('if test -e /etc/florrclone/shard-game.json || test -e /var/lib/florrclone-shards/authority.snapshot; then echo SHARDED; else echo LEGACY; fi').trim()==='LEGACY',
      'Shared-account servers require deploy-with-backup.cjs --cluster CONFIG');
    ssh('mkdir '+q(stateRoot+'/deploy.lock'));locked=true;
    old=ssh('readlink -f '+q(current)).trim();
    if (fresh || third) {
      const queuePolicy = third ? ['0'] : ['0','1'];
      assert(process.env.FLORR_PRESERVE_SAVE === '1' && queuePolicy.includes(process.env.FLORR_DOWNLOAD_QUEUE) &&
             process.env.FLORR_CLIENT_ASSET_HOST === target.host && !process.env.FLORR_ENTRY_PROXY_IP &&
             !process.env.FLORR_PASSWORD_RESET_FILE,
             'Independent instance requires an explicit download policy and its own preserved save');
      const command = ssh('systemctl show '+service+' --property=ExecStart --value');
      assert(command.includes(current+'/server.js') &&
             command.includes('--db '+databaseFile+' '),
             'Unexpected independent instance command or save');
      ssh('test -f '+q(nginxConfig)+' && test ! -e '+q(marker)+' && install -d -m 755 '+q(maintenance));
    }
    assert.match(old,/^\/opt\/florrclone\/releases\/[a-zA-Z0-9_-]+$/);
    if(process.env.FLORR_EXPECTED_RELEASE)assert.equal(old,process.env.FLORR_EXPECTED_RELEASE,'Online release changed; coordinate before deploying');
    verifyHello(backendPort, noticeConfig.protocolVersion, noticeConfig.contentHash);
    const nextProtocol = Number(process.env.FLORR_NEXT_PROTOCOL_VERSION);
    const nextHash = Number(process.env.FLORR_NEXT_CONTENT_HASH);
    assert(Number.isInteger(nextProtocol)&&nextProtocol>0&&nextProtocol<=65535);
    assert(Number.isInteger(nextHash)&&nextHash>=0&&nextHash<=0xffffffff);
    ssh('test -s '+q(release+'/server.wasm')+' && test -s '+q(release+'/bundle.wasm')+' && test -s '+q(release+'/deploy-runtime.cjs'));
    // Pause records must survive either branch. Only use them when both
    // exact binaries have passed the isolated resume/expiry test.
    if(process.env.FLORR_DROP_BONUS_PAUSE_PROOF) {
      const proof=JSON.parse(process.env.FLORR_DROP_BONUS_PAUSE_PROOF);
      for(const [dir,files] of [[old,proof.rollback],[release,proof.candidate]]) {
        for (const name of ['server.wasm','server.js','deploy-runtime.cjs']) {
          const hash=files?.[name];
          assert.match(hash,/^[a-f0-9]{64}$/);
          assert.equal(ssh('sha256sum '+q(dir+'/'+name)).split(/\s/)[0],hash,
            'Maintenance pause compatibility proof does not match the release');
        }
      }
      freezeDropBonus=true;
    }
    const backgroundSave=dir=>ssh('test -f '+q(dir+'/server.background-save')+' && echo enabled || true').trim()==='enabled';
    const retiringBackgroundSave=true, candidateBackgroundSave=true;
    let assetHashes;
    const assetMirror=process.env.FLORR_CLIENT_ASSET_MIRROR;
    if(process.env.FLORR_CLIENT_ASSET_HOST){
      for(const file of ['index.html',...clientAssets.assetNames])
        ssh('runuser -u www-data -- test -r '+q(release+'/'+file));
      assetHashes=clientAssets.readAssetHashes(ssh,release);
    }
    if(assetMirror){
      assert(assetHashes && process.env.FLORR_DOWNLOAD_QUEUE==='0','Mirror requires direct, versioned downloads');
      const url=clientAssets.mirrorAssetUrl(assetMirror,assetHashes);
      const digest=assetHashes['bundle.wasm.bin'];
      const check=`(async()=>{const r=await fetch(${JSON.stringify(url)},{redirect:'error',signal:AbortSignal.timeout(60000)});
        if(r.status!==200 || r.headers.get('access-control-allow-origin')!=='*' ||
            (r.headers.get('content-encoding')||'identity')!=='identity')throw Error('Mirror HTTP/CORS check failed');
        const size=Number(r.headers.get('content-length'));
        const hash=require('node:crypto').createHash('sha256');for await(const chunk of r.body)hash.update(chunk);
        if(hash.digest('hex')!==${JSON.stringify(digest)})throw Error('Mirror WASM SHA256 differs from candidate');
        const range=await fetch(${JSON.stringify(url)},{redirect:'error',headers:{Range:'bytes=0-0'},signal:AbortSignal.timeout(10000)});
        if(range.status!==206 || range.headers.get('access-control-allow-origin')!=='*' ||
            range.headers.get('content-range')!=='bytes 0-0/'+size || (await range.arrayBuffer()).byteLength!==1)
          throw Error('Mirror does not serve valid byte ranges');
        console.log('PASS public mirror WASM SHA256');})().catch(e=>{console.error(e.message);process.exitCode=1});`;
      console.log(ssh(runtime+' -e '+q(check)));
    }
    ssh('curl -fsS '+q(backend+'/api/health'));
    ssh('install -d -m 700 '+q(remote)+' && python3 --version >/dev/null');
    scp(archiveTool,host+':'+remoteArchiveTool);
    assert.equal(ssh('sha256sum '+q(remoteArchiveTool)).split(/\s/)[0],
      crypto.createHash('sha256').update(fs.readFileSync(archiveTool)).digest('hex'));
    const uploadedBonusHelper=remote+'/maintenance-drop-bonus.cjs';
    const helper=path.join(__dirname,freezeDropBonus?'stage-drop-bonus-pause.cjs':'maintenance-drop-bonus.cjs');
    scp(helper,host+':'+uploadedBonusHelper);
    assert.equal(ssh('sha256sum '+q(uploadedBonusHelper)).split(/\s/)[0],crypto.createHash('sha256').update(fs.readFileSync(helper)).digest('hex'));
    bonusHelper=uploadedBonusHelper;
    // Publish this release's Chinese notes before enabling maintenance.
    const notes=JSON.parse(fs.readFileSync(process.env.FLORR_RELEASE_NOTES_FILE||path.join(__dirname,'release-notes.json'),'utf8'));
    assert(Array.isArray(notes.changes)&&notes.changes.length>0,'Release notes are required');
    const escape=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const html=fs.readFileSync(path.join(__dirname,'maintenance.html'),'utf8').replace('<!-- RELEASE_NOTES -->',
      '<section style="text-align:left;font-size:14px"><h2>'+escape(notes.title)+'</h2><ul>'+notes.changes.map(t=>'<li>'+escape(t)+'</li>').join('')+'</ul></section>');
    const waitingFile=path.join(local,'maintenance.html');fs.writeFileSync(waitingFile,html);
    scp(waitingFile,host+':'+maintenance+'/maintenance.next.html');
    ssh('chmod 644 '+q(maintenance+'/maintenance.next.html')+' && mv '+q(maintenance+'/maintenance.next.html')+' '+q(maintenance+'/maintenance.html'));
    const frontendBefore=ssh('cat '+q(nginxConfig));
    const frontendSha=crypto.createHash('sha256').update(frontendBefore).digest('hex');
    const mirrorWasmSha256=process.env.FLORR_THIRD_WASM_BIN_SHA256;
    fs.writeFileSync(path.join(local,'nginx-client-assets.conf'),
        renderLiveFrontend(frontendBefore,{role,old,release,assetHashes,mirrorWasmSha256}));
    // Announce while players are still connected, before the maintenance gate.
    // Only an echoed Admin-channel broadcast counts as delivery confirmation.
    assert(!bootstrap && preserveSave,'Coordinated maintenance requires standard announcements and save preservation');
    await barrier('ready');
    const remoteAnnouncement = remote + '/deploy-announcement.cjs';
    const announcementPath=path.join(__dirname,'deploy-coordinated-announcement.cjs');
    scp(announcementPath,host+':'+remoteAnnouncement);
    assert.equal(ssh('sha256sum '+q(remoteAnnouncement)).split(/\s/)[0],
      crypto.createHash('sha256').update(fs.readFileSync(announcementPath)).digest('hex'));
    const remoteNotice={...noticeConfig,url:'ws://127.0.0.1:'+backendPort+'/ws'};
    const noticeCode=`const fs=require('node:fs');const a=require(${JSON.stringify(remoteAnnouncement)});
      (async()=>{const config=JSON.parse(fs.readFileSync(0,'utf8'));
      await a.countdown(async text=>{const c=await a.connect(config);
        try{await c.broadcast(text);console.log(text)}finally{c.close()}})})()
      .catch(e=>{console.error(e.message);process.exitCode=1});`;
    console.log(ssh(runtime+' -e '+q(noticeCode),JSON.stringify(remoteNotice)));
    // Serve the waiting page before gracefully stopping the old process.
    await barrier('announced');
    gated=true;
    bonusGatedAt=Number(ssh('gate_time=$(date +%s%3N) && touch '+q(marker)+' && printf %s "$gate_time"').trim());
    assert(Number.isSafeInteger(bonusGatedAt)&&bonusGatedAt>0,'Could not record maintenance gate closure');
    const waitingResponse=await fetch(publicEntry,{signal:AbortSignal.timeout(15000),cache:'no-store'});
    assert.equal(waitingResponse.status,503);
    const waiting=await waitingResponse.text();
    assert(waiting.includes('花朵躲石头'),'Waiting page must work before stopping');
    assert(waiting.includes(escape(notes.title)) && notes.changes.every(change=>waiting.includes('<li>'+escape(change)+'</li>')),'Current release notes must be visible before stopping');
    await barrier('gated');
    if (bootstrap) await new Promise(resolve=>setTimeout(resolve,60000));
    bonusStoppedAt=Number(ssh('date +%s%3N').trim());
    assert(Number.isSafeInteger(bonusStoppedAt)&&bonusStoppedAt>=bonusGatedAt);
    const retiringPid=ssh('systemctl show '+service+' -p MainPID --value').trim();
    assert.match(retiringPid,/^[1-9][0-9]*$/,'No running game process to save');
    stopped=true;
    bonusConfirmedStoppedAt=Number(ssh('systemctl stop '+service+' && date +%s%3N').trim());
    assert(Number.isSafeInteger(bonusConfirmedStoppedAt)&&bonusConfirmedStoppedAt>=bonusStoppedAt,
      'Could not confirm the game stop time');
    assert.equal(ssh('systemctl show '+service+' -p ExecMainStatus --value').trim(),'0','Game failed to stop cleanly');
    let saved;
    if(retiringBackgroundSave) {
      // The process can exit before journald drains its final stdout record.
      const journal='journalctl --boot -u '+service+' _PID='+retiringPid+
        ' -n 120 -o cat --no-pager | grep -m 1 -F '+q('[persist-shutdown]');
      const line=ssh('for attempt in $(seq 1 15); do '+journal+' && exit 0; sleep 1; done; exit 1').trim();
      const match=line.match(/generation=(\d+) accounts=(\d+) players=(\d+) bytes=(\d+) sha256=([0-9a-f]{64})$/);
      assert(match,'The retiring process did not confirm its final durable save');
      saved={generation:match[1],accounts:Number(match[2]),players:Number(match[3]),bytes:Number(match[4]),sha256:match[5]};
    }
    ssh('cp '+q(databaseFile)+' '+q(remote+'/inventory.json')+' && chmod 600 '+q(remote+'/inventory.json'));
    scp(host+':'+remote+'/inventory.json',path.join(local,'inventory.json'));
    const bytes=fs.readFileSync(path.join(local,'inventory.json'));
    const hash=crypto.createHash('sha256').update(bytes).digest('hex');
    assert.equal(ssh('sha256sum '+q(remote+'/inventory.json')).split(/\s/)[0],hash);
    const before=JSON.parse(bytes);
    if(saved) {
      assert.equal(hash,saved.sha256,'Backup SHA does not match the final save receipt');
      assert.equal(bytes.length,saved.bytes);
      assert.equal(Object.keys(before.users||{}).length,saved.accounts);
      assert.equal(Object.keys(before.players||{}).length,saved.players);
    }
    assert.equal(ssh('sha256sum '+q(databaseFile)).trim().split(/\s+/)[0],hash,'Stopped source changed during backup');
    verifiedBackupHash=hash;
    const serverArchives=JSON.parse(ssh('umask 077; test -f '+q(marker)+' && python3 '+
      q(remoteArchiveTool)+' pack '+q(remote)+' '+q(databaseFile)+' '+q(hash)));
    for(const name of archiveFiles)scp(host+':'+remote+'/'+name,path.join(local,name));
    const localArchives=JSON.parse(run('python',[archiveTool,'verify',local,path.join(local,'inventory.json'),hash]));
    assert.deepEqual(localArchives,serverArchives,'Local and remote archive checks differ');
    archiveProof=localArchives;
    const metadata={timestamp:stamp,previousRelease:old,nextRelease:release,sha256:hash,
      accounts:Object.keys(before.players||{}).length,remoteBackup:remote,transferArchives:archiveProof,...(saved&&{finalSave:saved})};
    fs.writeFileSync(path.join(local,'backup-metadata.json'),JSON.stringify(metadata,null,2));
    console.log('已验证本地最新存档备份：'+path.join(local,'inventory.json'));
    const transfer=await barrier('backup',{directory:local,sha256:hash,transferArchives:archiveProof,
      gatedAt:bonusGatedAt,stoppedAt:bonusStoppedAt,stopConfirmedAt:bonusConfirmedStoppedAt});
    // Preserve the original migration snapshot's fixed TP debt without
    // changing passwords, progress or inventory earned since the cutover.
    let next,report;
    if(preserveSave){
      assert(!process.env.FLORR_RESTORE_SNAPSHOT &&
        !process.env.FLORR_OPERATOR_HYPER_USER && process.env.FLORR_REFUND_ETERNAL_PRICE!=='1',
        'Preserve-save deployment cannot include account maintenance');
      next=path.join(local,'inventory.json');
      report={preservedSave:true};
      if (bootstrap) {
        next=path.join(local,'inventory.operator.json');
        report=JSON.parse(run(process.execPath,[path.join(__dirname,'prepare-deploy-operator.cjs'),path.join(local,'inventory.json'),process.env.FLORR_DEPLOY_ADMIN_FILE,next]));
      }
    }else if(process.env.FLORR_RESTORE_SNAPSHOT){
      const source=path.resolve(process.env.FLORR_RESTORE_SNAPSHOT);
      const bytes=fs.readFileSync(source);
      const expected=process.env.FLORR_RESTORE_SHA256;
      assert(expected && /^[a-f0-9]{64}$/.test(expected),'Explicit restore SHA256 required');
      assert.equal(crypto.createHash('sha256').update(bytes).digest('hex'),expected,'Restore snapshot hash mismatch');
      const restored=JSON.parse(bytes);assert(restored.users && restored.players,'Invalid restore database');
      next=path.join(local,'inventory.restored.json');fs.writeFileSync(next,bytes);
      report={restoreSnapshot:{source,sha256:expected,accounts:Object.keys(restored.players).length},accountAdjustments:[]};
      fs.writeFileSync(path.join(local,'snapshot-restore.json'),JSON.stringify(report.restoreSnapshot,null,2));
    }else{
    const baseline=path.join(root,'migrate/latest-final-cpp.json');
    next=path.join(local,'inventory.reconciled.json');
    report=JSON.parse(run(process.execPath,[path.join(__dirname,'reconcile-migrated-tp.js'),baseline,path.join(local,'inventory.json'),next]));
    const compare=JSON.parse(fs.readFileSync(next));
    for(const [id,p]of Object.entries(compare.players||{}))for(const k of ['talentPointBonus','talentPointReconciledAt']){
      if(k in before.players[id])p[k]=before.players[id][k];else delete p[k];
    }
    assert.deepEqual(compare,before);
    fs.writeFileSync(path.join(local,'tp-reconciliation.json'),JSON.stringify(report,null,2));
    // Explicitly requested releases, idempotent per account. This is AFTER
    // the stopped-save download/SHA gate and before the candidate is installed.
    const adjustments=path.join(root,'scripts/deferred-account-adjustments.json');
    if(fs.existsSync(adjustments)){
      const adjusted=path.join(local,'inventory.adjusted.json');
      report.accountAdjustments=JSON.parse(run(process.execPath,[path.join(__dirname,'release-selected-hypers.cjs'),next,adjusted,adjustments]));
      fs.writeFileSync(path.join(local,'account-adjustments.json'),JSON.stringify(report.accountAdjustments,null,2));
      next=adjusted;
    }
    const grantDb=JSON.parse(fs.readFileSync(next,'utf8'));
    report.requestedPetals=require('./grant-requested-petals.cjs').apply(grantDb);
    const granted=path.join(local,'inventory.granted.json');
    fs.writeFileSync(granted,JSON.stringify(grantDb));
    fs.writeFileSync(path.join(local,'requested-petals.json'),JSON.stringify(report.requestedPetals,null,2));
    next=granted;
    const shopManifest=path.join(__dirname,'manifests/shop-locks-20261104.json');
    if(fs.existsSync(shopManifest)){
      const restored=path.join(local,'inventory.shop-restored.json');
      report.shopLockRestore=JSON.parse(run(process.execPath,[path.join(__dirname,'restore-original-shop-locks.cjs'),'--input',next,'--output',restored,'--manifest',shopManifest]));
      fs.writeFileSync(path.join(local,'shop-lock-restore.json'),JSON.stringify(report.shopLockRestore,null,2));
      next=restored;
    }
    if(process.env.FLORR_REFUND_ETERNAL_PRICE==='1'){
      const refunded=path.join(local,'inventory.refunded.json');
      report.eternalRefund=JSON.parse(run(process.execPath,[path.join(__dirname,'refund-eternal-forging.cjs'),next,refunded]));
      fs.writeFileSync(path.join(local,'eternal-refund.json'),JSON.stringify(report.eternalRefund,null,2));
      next=refunded;
    }
    if(process.env.FLORR_OPERATOR_HYPER_USER){
      const queued=path.join(local,'inventory.operator.json');
      // Only the database directory is mounted into the WASM filesystem.
      const ready='/var/lib/florrclone/cpp-state/operator-ready-'+stamp;
      report.operatorRequest=JSON.parse(run(process.execPath,[path.join(__dirname,'prepare-operator-hyper-forge.cjs'),next,queued,process.env.FLORR_OPERATOR_HYPER_USER,'hammer-'+stamp,marker,ready]));
      fs.writeFileSync(path.join(local,'operator-request.json'),JSON.stringify(report.operatorRequest,null,2));
      next=queued;
    }
    }
    if(process.env.FLORR_PRIOR_MAINTENANCE_FILE){
      assert(process.env.FLORR_PRESERVE_SAVE==='1');
      const credit=JSON.parse(fs.readFileSync(process.env.FLORR_PRIOR_MAINTENANCE_FILE,'utf8'));
      assert(/^[a-zA-Z0-9_-]+$/.test(credit.id)&&Number.isSafeInteger(credit.durationMs)&&credit.durationMs>0&&credit.durationMs<3600000);
      const db=JSON.parse(fs.readFileSync(next,'utf8'));report.priorMaintenance={id:credit.id,extended:0,skipped:0};
      for(const row of credit.accounts){
        const bonus=db.players[row.accountId]?.dailyDropBonus;
        if(!bonus||bonus.day!==row.day||bonus.until!==row.until||bonus.until<=Date.now()||bonus.priorMaintenanceCredit===credit.id){report.priorMaintenance.skipped++;continue;}
        bonus.until+=credit.durationMs;bonus.priorMaintenanceCredit=credit.id;report.priorMaintenance.extended++;
      }
      next=path.join(local,'inventory.prior-maintenance.json');fs.writeFileSync(next,JSON.stringify(db));
    }
    if(process.env.FLORR_PETAL_GRANT_FILE){
      assert(process.env.FLORR_PRESERVE_SAVE==='1','Explicit petal grant requires preserve-save mode');
      const db=JSON.parse(fs.readFileSync(next,'utf8'));
      const request=JSON.parse(fs.readFileSync(process.env.FLORR_PETAL_GRANT_FILE,'utf8'));
      report.authorizedPetals=require('./grant-authorized-petals.cjs').apply(db,request);
      next=path.join(local,'inventory.authorized-petals.json');fs.writeFileSync(next,JSON.stringify(db));
      fs.writeFileSync(path.join(local,'authorized-petals.json'),JSON.stringify(report.authorizedPetals,null,2));
    }
    if(process.env.FLORR_COUNCIL_HISTORY_FILE){
      assert(process.env.FLORR_PRESERVE_SAVE==='1','History import requires preserve-save mode');
      const saved=JSON.parse(fs.readFileSync(next,'utf8'));
      const seed=JSON.parse(fs.readFileSync(process.env.FLORR_COUNCIL_HISTORY_FILE,'utf8'));
      report.councilHistory=require('./merge-council-history.cjs').apply(saved,seed);
      const historyFile=path.join(local,'inventory.history.json');fs.writeFileSync(historyFile,JSON.stringify(saved));next=historyFile;
    }
    if(councilManagerId){
      const saved=JSON.parse(fs.readFileSync(next,'utf8'));
      const {prepareCouncilManagers}=require('./prepare-council-managers.cjs');
      const delegated=prepareCouncilManagers(saved,[councilManagerId]);
      next=path.join(local,'inventory.council-delegation.json');
      fs.writeFileSync(next,JSON.stringify(delegated),{mode:0o600});
      report.councilDelegation=true;
    }
    if(resetRequests){
      const reset=path.join(local,'inventory.password-reset.json');
      report.passwordReset=JSON.parse(run(process.execPath,
        [path.join(__dirname,'reset-password-offline.cjs'),next,reset,'-'],JSON.stringify(resetRequests)));
      next=reset;
      fs.writeFileSync(path.join(local,'password-reset.json'),JSON.stringify(report.passwordReset,null,2));
    }
    assert.equal(next,path.join(local,'inventory.json'),'Unexpected concurrent save operation');
    assert.equal(transfer.sourceSHA256,hash,'Migration source does not match this stopped save');
    next=path.join(coordinated,role+'-candidate.json');
    assert.equal(crypto.createHash('sha256').update(fs.readFileSync(next)).digest('hex'),transfer.candidateSHA256);
    report.accountMigration=true;
    if (preserveSave && next === path.join(local,'inventory.json')) {
      ssh('cp '+q(remote+'/inventory.json')+' '+q(remote+'/inventory.next.json')+
          ' && printf "%s\\n" '+q(hash+'  '+remote+'/inventory.next.json')+' | sha256sum -c --status');
    } else {
      const compressed=path.join(local,'inventory.candidate.json.gz');
      const pipeline=require('node:stream/promises').pipeline;
      await pipeline(fs.createReadStream(next),require('node:zlib').createGzip({level:1}),
        fs.createWriteStream(compressed,{flags:'wx',mode:0o600}));
      const targetZip=remote+'/inventory.candidate.json.gz';
      const targetSave=remote+'/inventory.next.json';
      scp(compressed,host+':'+targetZip);
      ssh('umask 077; gzip -dc '+q(targetZip)+' >'+q(targetSave+'.tmp')+
          ' && mv '+q(targetSave+'.tmp')+' '+q(targetSave));
    }
    if(transfer||resetRequests||councilManagerId||bootstrap){
      const candidateHash=crypto.createHash('sha256').update(fs.readFileSync(next)).digest('hex');
      assert.equal(ssh('sha256sum '+q(remote+'/inventory.next.json')).split(/\s/)[0],candidateHash,
        'Save candidate SHA256 mismatch');
    }
    // All validation and the local backup gate have completed at this point.
    saveInstallStarted=true;
    ssh('install -o '+installOwner+' -g '+installOwner+' -m 600 '+q(remote+'/inventory.next.json')+' '+q(databaseNext));
    ssh('mv '+q(databaseNext)+' '+q(databaseFile));
    switchAttempted=true;
    ssh('ln -s '+q(release)+' '+q(current+'.next')+' && mv -Tf '+q(current+'.next')+' '+q(current));
    report.maintenanceBonus=preserveMaintenanceBonus();
    ssh('systemctl start '+service);
    let healthy=false;
    for(let i=0;i<40;i++){
      try{const h=JSON.parse(ssh('curl -fsS '+q(backend+'/api/health')));if(h.ok){healthy=true;break;}}catch{}
      await new Promise(r=>setTimeout(r,1000));
    }
    assert(healthy,'New service failed health check');
    if(candidateBackgroundSave) {
      const pid=ssh('systemctl show '+service+' -p MainPID --value').trim();
      assert.match(pid,/^[1-9][0-9]*$/);
      ssh('for attempt in $(seq 1 75); do journalctl --boot -u '+service+' _PID='+pid+
        ' --no-pager -o cat --grep '+q('\\[persist-worker\\] saved generation=')+
        ' | tail -1 | grep -q generation= && exit 0; sleep 1; done; exit 1');
    }
    for(const file of ['','bundle.js','bundle.wasm','bundle.js.bin','bundle.wasm.bin'])
      ssh('curl -fsS -o /dev/null '+q(backend+'/'+file));
    verifyHello(backendPort,nextProtocol,nextHash);
    const snapshotPid=ssh('systemctl show '+service+' -p MainPID --value').trim();
    assert.match(snapshotPid,/^[1-9][0-9]*$/);
    assert.equal(ssh('cat '+q(release+'/server.snapshot-rate')).trim(),'20','Staged snapshot rate must be 20 Hz');
    const snapshotEnvironment=ssh('tr '+q('\\0')+' '+q('\\n')+' < /proc/'+snapshotPid+'/environ | grep -m1 '+q('^FLIX_SNAPSHOT_RATE=')+' || true').trim();
    assert(['','FLIX_SNAPSHOT_RATE=20'].includes(snapshotEnvironment),'Service environment overrides 20 Hz snapshot rate');
    if(process.env.FLORR_REQUIRE_COUNCIL==='1') {
      ssh('test -s '+q(release+'/council.html')+' && curl -fsS -o /dev/null '+q(backend+'/council.html'));
      const councilUrl = JSON.stringify(backend+'/api/council');
      const check="(async()=>{const r=await fetch("+councilUrl+",{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'view'})});if(!r.ok)throw Error('Council HTTP '+r.status);const j=await r.json();if(j.error||!Array.isArray(j.players)||!Array.isArray(j.polls))throw Error('Invalid council state');if(j.players.length&&j.players.reduce((s,p)=>s+p.units,0)!==1000000)throw Error('Invalid council weights');console.log('PASS council API and exact weight total');})().catch(e=>{console.error(e.message);process.exitCode=1});";
      console.log(ssh(runtime+' -e '+q(check)));
    }
    if (bootstrap) {
      const newHash=Number(process.env.FLORR_NEXT_CONTENT_HASH);
      assert(Number.isInteger(newHash) && newHash>=0 && newHash<=0xffffffff, 'Candidate content hash required for operator validation');
      const version = Number(process.env.FLORR_NEXT_PROTOCOL_VERSION || noticeConfig.protocolVersion);
      assert(Number.isInteger(version) && version > 0 && version <= 65535, 'Invalid candidate protocol');
      const candidate={...noticeConfig,protocolVersion:version,contentHash:newHash,url:'ws://127.0.0.1:'+backendPort+'/ws'};
      const verify=`const fs=require('fs');(async()=>{const a=await require(${JSON.stringify(release+'/deploy-announcement.cjs')}).connect(JSON.parse(fs.readFileSync(0,'utf8')));try{await a.broadcast('维护更新验证完成，即将开放入口。');console.log('PASS operator Admin broadcast');}finally{a.close();}})().catch(e=>{console.error(e.message);process.exitCode=1});`;
      console.log(cp.execFileSync('ssh',[...sshOptions,host,runtime+' -e '+q(verify)],{input:JSON.stringify(candidate),encoding:'utf8',timeout:30000}));
    }
    if(process.env.FLORR_DOWNLOAD_QUEUE==='1'){
      assert(process.env.FLORR_CLIENT_ASSET_HOST,'Queue requires a versioned asset host');
      const unit=path.join(local,'florr-download-queue.service');
      const assetBase='http://'+process.env.FLORR_CLIENT_ASSET_HOST+':'+publicPort+'/client-assets/'+release.split('/').pop()+'/';
      const slots=Number(process.env.FLORR_DOWNLOAD_SLOTS||8);
      assert(Number.isInteger(slots)&&slots>=1&&slots<=32,'Invalid download slot count');
      fs.writeFileSync(unit,'[Unit]\nDescription=Florr client download queue\nAfter=network.target\n[Service]\nUser=florrclone\nEnvironment=PORT=8789\nEnvironment=DOWNLOAD_SLOTS='+slots+'\nEnvironment=DOWNLOAD_ASSET_BASE='+assetBase+'\nExecStart=/opt/florrclone/runtime/bin/node '+release+'/download-queue.cjs\nRestart=on-failure\nNoNewPrivileges=true\nProtectSystem=strict\nProtectHome=true\n[Install]\nWantedBy=multi-user.target\n');
      ssh('test -s '+q(release+'/download-queue.cjs')+' && if test -f /etc/systemd/system/florr-download-queue.service; then cp /etc/systemd/system/florr-download-queue.service '+q(remote+'/download-queue-before.service')+'; fi');
      scp(unit,host+':/etc/systemd/system/florr-download-queue.service');
      ssh('systemctl daemon-reload && systemctl enable florr-download-queue && systemctl restart florr-download-queue');
      let queueReady=false;for(let i=0;i<15;i++){try{assert(JSON.parse(ssh('curl -fsS http://127.0.0.1:8789/health')).ok);queueReady=true;break;}catch{await new Promise(r=>setTimeout(r,1000));}}
      assert(queueReady,'Download queue unavailable; keep maintenance closed');
    }
    {
      const nginx=path.join(local,'nginx-client-assets.conf');
      assert.equal(ssh('sha256sum '+q(nginxConfig)).split(/\s/)[0],frontendSha,'Public vhost changed after preflight');
      installNginx(nginx,remote+'/nginx-before.conf');
    }
    const assetUrl='http://127.0.0.1:'+publicPort;
    const wasmPath=clientAssets.cacheAssetPath('bundle.wasm.bin',assetHashes);
    const servedCommand='set -o pipefail; curl -fsS '+q(assetUrl+wasmPath)+' | sha256sum';
    if(third){
      assert.equal(ssh(servedCommand).split(/\s/)[0],assetHashes['bundle.wasm.bin'],'Served bundle SHA differs');
      const rangeHeaders=ssh('curl -fsS -H '+q('Range: bytes=0-63')+' -H '+
        q('Origin: http://127.0.0.1:9999')+' -D - -o /dev/null '+q(assetUrl+wasmPath));
      assert(/HTTP\/1\.[01] 206\b/.test(rangeHeaders)&&/cache-control: [^\r\n]*immutable/i.test(rangeHeaders)&&
        /access-control-allow-origin: \*/i.test(rangeHeaders),'Immutable WASM Range or CORS response differs');
    }else{
      const backendBundle='http://127.0.0.1:'+backendPort+'/bundle.wasm.bin';
      assert.equal(ssh('set -o pipefail; curl -fsS '+q(backendBundle)+' | sha256sum').split(/\s/)[0],
        assetHashes['bundle.wasm.bin'],'Backend bundle SHA differs');
    }
    // The command can take effect even if its SSH acknowledgement is lost.
    await barrier('installed',{directory:local});
    assert(fs.existsSync(path.join(coordinated,'commit')),'Coordinator has not committed');
    mayHaveOpened=true;
    ssh('rm '+q(marker));
    verifyHello(publicPort,nextProtocol,nextHash);
    if(!third) assert.equal(ssh(servedCommand).split(/\s/)[0],assetHashes['bundle.wasm.bin'],'Served bundle SHA differs');
    for(const uri of ['/','/index.html','/?_flix_recover=deploy-check']){
      const html=ssh('curl -fsS '+q(assetUrl+uri));
      const tag=(html.match(/<meta\b[^>]*>/gi)||[]).find(item=>
        /\bname\s*=\s*["']?florr-client-assets(?=["'\s>])/i.test(item));
      const meta=tag?.match(/\bcontent\s*=\s*["']?([A-Za-z0-9_-]+)/i);
      assert(meta && /data:image\/svg\+xml;base64,/.test(html),'Loaded homepage is missing asset routing or background');
      const routing=JSON.parse(Buffer.from(meta[1],'base64url').toString());
      for(const file of clientAssets.assetNames)
        assert.equal(routing.assets[file],clientAssets.cacheAssetPath(file,assetHashes),'Loaded homepage references a different '+file);
    }
    if(report.operatorRequest)ssh('touch '+q(report.operatorRequest.readyFile)+' && chmod 644 '+q(report.operatorRequest.readyFile));
    if(process.env.FLORR_NEXT_CONTENT_HASH){
      const nextHash=Number(process.env.FLORR_NEXT_CONTENT_HASH);assert(Number.isInteger(nextHash)&&nextHash>=0&&nextHash<=0xffffffff);
      fs.writeFileSync(process.env.FLORR_DEPLOY_ADMIN_FILE,JSON.stringify({...noticeConfig,
        protocolVersion:Number(process.env.FLORR_NEXT_PROTOCOL_VERSION || noticeConfig.protocolVersion),
        contentHash:nextHash}));
    }
    fs.writeFileSync(path.join(local,'deployment-result.json'),JSON.stringify({...metadata,...report,status:'deployed'},null,2));
    console.log(JSON.stringify({status:'deployed',backup:local,...report}));
  }catch(error){
    const commit=fs.existsSync(path.join(coordinated,'commit'));
    const engaged=fs.existsSync(path.join(coordinated,role+'.installed'));
    const abort=fs.existsSync(path.join(coordinated,'abort'));
    // Once we have told the parent installation succeeded, a local timeout
    // is not permission to restore if the parent has yet to decide.
    if(mayHaveOpened||commit||(engaged&&!abort)){
      // Players may already have earned progress: never restore the pre-cutover
      // save after reopening. Keep this verified backend and close the gate for
      // diagnosis if the final public probe fails.
      try {
        ssh('touch '+q(marker));
        console.error('Public verification failed or gate opening is uncertain; current save retained behind maintenance.');
      } catch(recovery) {
        console.error('Could not confirm maintenance gate; current save retained:',recovery.message);
      }
    }else if(stopped){
      ssh('if test -s '+q(remote+'/nginx-before.conf')+'; then cp '+q(remote+'/nginx-before.conf')+' '+q(nginxConfig)+' && nginx -t && systemctl reload nginx; fi');
      if(process.env.FLORR_DOWNLOAD_QUEUE==='1'){
        ssh('if test -s '+q(remote+'/download-queue-before.service')+'; then cp '+q(remote+'/download-queue-before.service')+' /etc/systemd/system/florr-download-queue.service && systemctl daemon-reload && systemctl restart florr-download-queue; else systemctl disable --now florr-download-queue || true; fi');
      }

      // The maintenance gate has kept players out throughout this attempt.
      ssh('touch '+q(marker));
      ssh('systemctl stop '+service);
      const actualRelease=ssh('readlink -f '+q(current)).trim();
      assert(actualRelease===old||(switchAttempted&&actualRelease===release),'Unexpected active release; preserve the current save for review');
      if(saveInstallStarted){
        const snapshot=remote+'/inventory.json';
        assert.match(verifiedBackupHash,/^[a-f0-9]{64}$/);
        assert.equal(ssh('sha256sum '+q(snapshot)).trim().split(/\s+/)[0],verifiedBackupHash,'Rollback backup failed verification');
        assert(archiveProof,'Missing transfer archives backup proof');
        assert.equal(ssh('sha256sum '+q(remote+'/transfer-archives-manifest.json')).split(/\s/)[0],
          archiveProof.manifestSha256,'Rollback transfer archive manifest SHA-256 mismatch');
        const quarantine=path.posix.join(path.posix.dirname(databaseFile),'.transfer-archives-candidate-'+stamp);
        const recoveredArchives=JSON.parse(ssh('umask 077; test -f '+q(marker)+' && python3 '+
          q(remoteArchiveTool)+' restore '+[remote,databaseFile,verifiedBackupHash,snapshot,quarantine].map(q).join(' ')));
        fs.writeFileSync(path.join(local,'transfer-archives-rollback.json'),JSON.stringify(recoveredArchives));
        const restoreTemp=databaseFile+'.rollback-'+stamp;
        ssh('install -o '+installOwner+' -g '+installOwner+' -m 600 '+q(snapshot)+' '+q(restoreTemp)+
            ' && mv -Tf '+q(restoreTemp)+' '+q(databaseFile));
      }
      if(actualRelease!==old)ssh('ln -sfnT '+q(old)+' '+q(current+'.rollback')+' && mv -Tf '+q(current+'.rollback')+' '+q(current));
      if(bonusHelper&&verifiedBackupHash)preserveMaintenanceBonus();
      ssh('systemctl start '+service);
    }
    if(gated&&!mayHaveOpened) {
      try {
        for(let trial=0;trial<18;trial++) {
          try { ssh('curl -fsS '+q(backend+'/api/health')); break; }
          catch(e) { if(trial===17) throw e; await new Promise(r=>setTimeout(r,1000)); }
        }
        const currentRelease=ssh('readlink -f '+q(current)).trim();
        assert.equal(currentRelease,old);
        verifyHello(backendPort,noticeConfig.protocolVersion,noticeConfig.contentHash);
        ssh('rm -f '+q(marker));
        console.error('Recovered prior healthy release.');
      } catch(recovery) { console.error('Maintenance retained pending recovery:',recovery.message); }
    }
    throw error;
  }finally{if(locked)ssh('rmdir '+q(stateRoot+'/deploy.lock'));}
}
main().catch(e=>{console.error(e.message);process.exitCode=1;});
