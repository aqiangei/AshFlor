'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
module.exports=function createBarrier(root,role){
  const folder=path.resolve(root);
  assert(['first','second','third'].includes(role));
  return async function barrier(phase, detail={}){
    const signal=path.join(folder,role+'.'+phase);
    const temporary=signal+'.tmp';
    fs.writeFileSync(temporary,JSON.stringify(detail),{flag:'wx'});
    fs.renameSync(temporary,signal);
    const deadline=Date.now()+12*60*1000;
    const release=path.join(folder,role+'.'+phase+'.go');
    for(;;){
      if(fs.existsSync(path.join(folder,'abort'))&&!fs.existsSync(path.join(folder,'commit'))) throw Error('Coordinated maintenance aborted');
      if(fs.existsSync(release)) return JSON.parse(fs.readFileSync(release,'utf8'));
      if(Date.now()>=deadline)throw Error('Coordinated maintenance phase timed out: '+phase);
      await new Promise(resolve=>setTimeout(resolve,250));
    }
  };
};
