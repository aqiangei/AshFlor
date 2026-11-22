'use strict';
const assert = require('node:assert/strict');

function stageDropBonusPause(database, { id, gatedAt }) {
  assert(typeof id === 'string' && /^[a-zA-Z0-9_-]{1,100}$/.test(id));
  assert(Number.isSafeInteger(gatedAt) && gatedAt > 0);
  assert(database?.players && typeof database.players === 'object' && !Array.isArray(database.players));
  const gateDay = Math.floor((gatedAt + 8 * 3_600_000) / 86_400_000);
  let paused = 0;
  for (const player of Object.values(database.players)) {
    const bonus = player?.dailyDropBonus;
    if (!bonus || bonus.day !== gateDay || bonus.pausedMillis > 0 || bonus.maintenancePause) continue;
    if ((bonus.multiplier !== 2 && bonus.multiplier !== 3) ||
        !Number.isSafeInteger(bonus.until) || bonus.until <= gatedAt) continue;
    const maximum = (bonus.multiplier === 3 ? 4 : 3) * 3_600_000;
    bonus.maintenancePause = { id, remainingMillis: Math.min(bonus.until - gatedAt, maximum) };
    bonus.until = 0;
    paused++;
  }
  return { id, gatedAt, paused };
}
module.exports = { stageDropBonusPause };

if (require.main === module) {
  const fs = require('node:fs');
  const [file,id,gate] = process.argv.slice(2);
  const data = JSON.parse(fs.readFileSync(file,'utf8'));
  const result = stageDropBonusPause(data,{id,gatedAt:Number(gate)});
  if (result.paused) {
    const temporary = file+'.bonus-pause-'+require('node:crypto').randomUUID();
    const stat = fs.statSync(file);
    const fd = fs.openSync(temporary,'wx',stat.mode&0o777);
    try {
      try {
        fs.writeFileSync(fd,JSON.stringify(data));
        if (process.platform!=='win32') fs.fchownSync(fd,stat.uid,stat.gid);
        fs.fsyncSync(fd);
      } finally { fs.closeSync(fd); }
      fs.renameSync(temporary,file);
    } catch (error) {
      fs.rmSync(temporary,{force:true});
      throw error;
    }
    if (process.platform!=='win32') {
      const dir = fs.openSync(require('node:path').dirname(file),'r');
      try { fs.fsyncSync(dir); } finally { fs.closeSync(dir); }
    }
  }
  console.log(JSON.stringify(result));
}
