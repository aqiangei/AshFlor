'use strict';
(async () => {
  const realm = /^\/transfer\/([123])\/$/.exec(location.pathname)?.[1];
  if (!realm || !isSecureContext) return;
  const names = ['','一服','二服','三服'];
  const games = ['','http://202.189.8.109:67/','http://202.189.23.253:67/','http://103.91.210.16:67/'];
  const path = '/transfer/' + realm;
  const endpoint = path + '/api/account-transfer';
  const storageKey = 'ashflor.transfer.portal.' + realm;
  const login = document.querySelector('#login');
  const transfer = document.querySelector('#transfer');
  const status = document.querySelector('[role=status]');
  const done = document.querySelector('#done');
  const target = transfer.elements.realm;
  let token = '';
  let sourceSettings = '';
  let pending;
  document.querySelector('#source').textContent = '原服：' + names[realm];
  const messages = {
    username_taken:'目标服已有该账号，请换一个名称。',
    invalid_username:'目标账号名称不符合要求。',
    invalid_password:'目标账号密码不符合要求。',
    login_required:'请先登录原服。',
    ineligible_account:'该账号暂不支持自助转服。',
    no_character:'原服没有可迁移的角色。',
    transfer_cooldown:'转服冷却尚未结束，请稍后再试。',
    transfer_capacity:'转服服务当前繁忙，请稍后再试。',
    transfer_forge_review:'原服仍持有锻造名额，请刷新页面并确认解除规则后转服。',
    password_mismatch:'请填写首次提交时设置的目标账号密码。',
    cancelled:'该笔转服已撤销，可重新登录原服。',
  };
  const encode = new TextEncoder();
  const decode = new TextDecoder('utf-8',{fatal:true});

  function signIn(version, username, password) {
    const values = [encode.encode(username),encode.encode(password)];
    if (values[0].length > 64 || values[1].length > 72) return Promise.reject(new Error('账号或密码过长。'));
    const socket = new WebSocket('wss://' + location.host + path + '/ws','binary');
    socket.binaryType = 'arraybuffer';
    return new Promise((resolve,reject) => {
      let finished = false;
      function close(error,value) {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        socket.close();
        if (error) reject(error); else resolve(value);
      }
      const timer = setTimeout(() => close(new Error('登录超时，请重试。')),12000);
      function send(body) {
        const data = new Uint8Array(body.length+4);
        new DataView(data.buffer).setUint32(0,body.length,true);
        data.set(body,4);
        socket.send(data);
      }
      socket.addEventListener('open',() => {
        const hello = new Uint8Array(7);
        const view = new DataView(hello.buffer);
        hello[0] = 1;
        view.setUint16(1,version.protocol,true);
        view.setUint32(3,version.contentHash,true);
        send(hello);
      });
      socket.addEventListener('message',event => {
        try {
          const buffer = new DataView(event.data);
          for (let p=0; p+5<=buffer.byteLength;) {
            const length = buffer.getUint32(p,true);
            const i = p+4;
            if (!length || i+length>buffer.byteLength) throw Error('服务器响应格式异常。');
            const tag = buffer.getUint8(i);
            if (tag===1) {
              if (length<4 || buffer.getUint8(i+3)!==1) throw Error('服务器版本正在切换，请刷新重试。');
              const auth = new Uint8Array(1+values.reduce((n,v)=>n+2+v.length,0));
              const view = new DataView(auth.buffer);
              let cursor = 1;
              auth[0] = 3;
              for (const value of values) {
                view.setUint16(cursor,value.length,true);
                auth.set(value,cursor+2);
                cursor += 2+value.length;
              }
              send(auth);
            } else if (tag===2) {
              if (length<4 || buffer.getUint8(i+1)!==0) throw Error('登录失败。请检查原服账号、密码或转服状态。');
              const size = buffer.getUint16(i+2,true);
              if (!size || size>512 || size+4>length) throw Error('登录响应格式异常。');
              return close(null,decode.decode(new Uint8Array(event.data,i+4,size)));
            }
            p = i+length;
          }
        } catch (e) { close(e); }
      });
      socket.addEventListener('error',()=>close(new Error('连接失败，请稍后重试。')));
      socket.addEventListener('close',()=>close(new Error('连接中断，请重试。')));
    });
  }

  function showTransfer() {
    const confirmation = transfer.elements.confirmPassword;
    confirmation.disabled = Boolean(pending);
    confirmation.closest('label').hidden = Boolean(pending);
    if (pending) confirmation.value = '';
    login.hidden = true;
    transfer.hidden = false;
    if (pending) {
      target.value = pending.realm;
      target.disabled = true;
      transfer.elements.username.value = pending.username;
      transfer.elements.username.readOnly = true;
      status.textContent = '有一笔申请待确认，请填写首次提交时设置的目标服密码。';
    } else {
      target.disabled = false;
      transfer.elements.username.readOnly = false;
    }
  }

  let version;
  const opener = window.opener;
  if (opener) {
    const sourceOrigin = new URL(games[Number(realm)]).origin;
    const challenge = Array.from(crypto.getRandomValues(new Uint8Array(16)),
      byte => byte.toString(16).padStart(2,'0')).join('');
    const acceptSession = event => {
      if (event.source !== opener || event.origin !== sourceOrigin ||
          event.data?.type !== 'ashflor-transfer-session' || event.data.realm !== realm ||
          event.data.challenge !== challenge || typeof event.data.token !== 'string' ||
          !/^[a-f0-9]{64}$/.test(event.data.token)) return;
      window.removeEventListener('message',acceptSession);
      clearTimeout(handoffTimeout);
      token = event.data.token;
      if (typeof event.data.settings === 'string' && event.data.settings.length <= 4096 &&
          /^[A-Za-z0-9+/=]+$/.test(event.data.settings)) sourceSettings = event.data.settings;
      window.opener = null;
      if (version) {
        status.textContent = '已带入原服登录态。';
        showTransfer();
      }
    };
    window.addEventListener('message',acceptSession);
    const handoffTimeout = setTimeout(() => window.removeEventListener('message',acceptSession),15000);
    opener.postMessage({type:'ashflor-transfer-ready',realm,challenge},sourceOrigin);
  }
  try {
    const responses = await Promise.all([fetch('/transfer/release.json',{cache:'no-store'}),fetch(endpoint,{cache:'no-store'})]);
    if (responses.some(r=>!r.ok)) throw Error();
    const [around,config] = await Promise.all(responses.map(r=>r.json()));
    if (config.realm!==realm || !Number.isInteger(around.protocol) || !Number.isInteger(around.contentHash)) throw Error();
    version = around;
    for (const id of Object.keys(config.targets || {})) {
      if (!/^[123]$/.test(id) || id===realm) continue;
      target.append(new Option(names[id],id));
    }
    if (!target.options.length) throw Error();
    pending = JSON.parse(localStorage.getItem(storageKey)||'null');
    if (pending && (!/^[a-f0-9]{64}$/.test(pending.receipt)||!Object.hasOwn(config.targets,pending.realm))) throw Error();
    if (token) status.textContent = '已带入原服登录态。';
    if (pending || token) showTransfer(); else login.hidden = false;
  } catch (_) {
    status.textContent = '转服入口暂未开放，请稍后刷新。';
    return;
  }

  login.addEventListener('submit',async event=>{
    event.preventDefault();
    const button=login.querySelector('button');
    button.disabled=true;
    try {
      token=await signIn(version,login.elements.username.value.trim(),login.elements.password.value);
      status.textContent='已登录原服。';
      showTransfer();
    } catch (e) { status.textContent=e.message; }
    finally { login.elements.password.value=''; button.disabled=false; }
  });

  transfer.addEventListener('submit',async event=>{
    event.preventDefault();
    const button=transfer.querySelector('button');
    if (button.disabled) return;
    const confirmation = transfer.elements.confirmPassword;
    if (!pending && (!confirmation.value || confirmation.value !== transfer.elements.password.value)) {
      status.textContent = '两次输入的目标服新密码不一致，请重新确认。';
      confirmation.focus();
      confirmation.select();
      return;
    }
    button.disabled=true;
    try {
      if (!pending) {
        const bytes=crypto.getRandomValues(new Uint8Array(32));
        const receipt=Array.from(bytes,n=>n.toString(16).padStart(2,'0')).join('');
        pending={realm:target.value,username:transfer.elements.username.value.trim(),receipt};
        localStorage.setItem(storageKey,JSON.stringify(pending));
        showTransfer();
      }
      status.textContent='正在办理转服。';
      const requestTransfer = async () => (await fetch(endpoint,{
        method:'POST',cache:'no-store',
        headers:{'Content-Type':'application/json',Authorization:'Bearer '+token},
        body:JSON.stringify({...pending,password:transfer.elements.password.value}),
      })).json();
      let result = await requestTransfer();
      if (result.error === 'transfer_forge_review' && !pending.releaseForgeClaims) {
        if (window.confirm('原服的独占锻造名额会解除，对应花瓣会永久销毁，材料不退。确定继续转服吗？')) {
          pending.releaseForgeClaims = true;
          localStorage.setItem(storageKey,JSON.stringify(pending));
          result = await requestTransfer();
        } else result = {error:'cancelled'};
      }
      if (result.ok) {
        const gameUrl = new URL(games[Number(pending.realm)]);
        if (sourceSettings) gameUrl.hash = 'flix-settings=' + sourceSettings;
        done.href=gameUrl.href;
        localStorage.removeItem(storageKey);
        pending=null;
        token='';
        transfer.hidden=true;
        done.hidden=false;
        status.textContent='转服完成，正在打开目标服。请使用新账号和密码登录。';
        location.assign(done.href);
      } else {
        if (['username_taken','invalid_username','invalid_password','cancelled','login_required','ineligible_account','no_character','transfer_cooldown','transfer_capacity','transfer_forge_review'].includes(result.error)) {
          localStorage.removeItem(storageKey);
          pending=null;
          if (result.error==='login_required') token='';
          if (!token) {transfer.hidden=true;login.hidden=false;} else showTransfer();
        }
        status.textContent=messages[result.error]||'该笔申请正在确认，请使用同一密码重试。';
      }
    } catch (_) {
      status.textContent='未收到结果，请使用首次设置的目标服密码重试。';
    } finally {
      transfer.elements.password.value='';
      confirmation.value='';
      button.disabled=false;
    }
  });
})();
