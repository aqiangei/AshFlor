'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');

const names = ['bundle.js', 'bundle.wasm', 'bundle.js.bin', 'bundle.wasm.bin'];
const markers = {
  first: '/var/www/florrclone-maintenance/enabled',
  second: '/var/www/ashflor-direct-maintenance/enabled',
  third: '/var/lib/florr-third/maintenance/enabled',
};
const tokenPattern = /sub_filter 'FLIX_CLIENT_ASSET_ROUTING' '([A-Za-z0-9_-]+={0,2})';/g;
const releasePath = /^\/opt\/florrclone\/releases\/[a-zA-Z0-9_-]+$/;
const regexp = s => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
const json = object => JSON.stringify(object);

function replaceOnce(text, old, replacement, description) {
  const position = text.indexOf(old);
  assert(position >= 0 && position === text.lastIndexOf(old), description);
  return text.slice(0, position) + replacement + text.slice(position + old.length);
}

function location(conf, name, ...qualifier) {
  const prefix = '    location ' + [...qualifier, name].join(' ') + ' {';
  const pattern = new RegExp('^' + regexp(prefix) + '\\r?\\n[\\s\\S]*?^    }(?=\\r?$)', 'gm');
  const found = [...conf.matchAll(pattern)];
  assert.equal(found.length, 1, 'Expected one nginx location: ' + name);
  return found[0][0];
}

function hasExact(conf, uri) {
  return new RegExp('^    location = ' + regexp(uri) + ' \\{', 'm').test(conf);
}

function readRouting(conf) {
  const encodings = [...conf.matchAll(tokenPattern)].map(m => m[1]);
  assert(encodings.length === 2 && encodings[0] === encodings[1],
    'Both homepage routing payloads must agree');
  return JSON.parse(Buffer.from(encodings[0], 'base64url').toString('utf8'));
}

function checkFrontendHtml(html) {
  const tags = (html.match(/<meta\b[^>]*>/gi)||[]).filter(tag =>
    /\bname\s*=\s*["']?florr-client-assets(?=["'\s>])/i.test(tag) &&
    /\bcontent\s*=\s*["']?FLIX_CLIENT_ASSET_ROUTING(?=["'\s>])/.test(tag));
  assert.equal(tags.length, 1, 'index must contain one asset routing filter');
  assert(!/FLIX_LOADING_GROUND/.test(html) && /background:[^;{}]*url\(data:image\/svg\+xml;base64,/.test(html),
    'missing rendered startup background in index');
}

// Only stage generated config through the deployment barrier. This function never performs I/O.
function renderLiveFrontend(before, {role, old, release, assetHashes, mirrorWasmSha256}) {
  assert(Object.hasOwn(markers, role), 'Unknown role');
  assert(releasePath.test(old) && releasePath.test(release) && release !== old, 'Unexpected release directories');
  const marker = markers[role];
  assert(before.includes('/api/server-status') && before.includes('/server-guide-live.js'),
    'Missing independent server guide routes');
  const existing = readRouting(before);
  const oldBase = '/client-assets/' + path.posix.basename(old) + '/';
  const assetBase = '/client-assets/' + path.posix.basename(release) + '/';
  assert.equal(existing.assetBase, oldBase, 'Vhost does not advertise expected retiring release');
  assert.equal(Object.keys(existing.assets).length, names.length, 'Unexpected asset list');

  const assets = {};
  for (const name of names) {
    assert(new RegExp('^/client-assets/sha256-[a-f0-9]{64}/' + regexp(name) + '$').test(existing.assets[name]),
      'Unexpected prior asset location: ' + name);
    assert(/^[a-f0-9]{64}$/.test(assetHashes[name]), 'Missing new digest: ' + name);
    assets[name] = '/client-assets/sha256-' + assetHashes[name] + '/' + name;
  }
  const next = { ...existing, assetBase, assets };
  if (role === 'third') {
    assert(!Object.hasOwn(existing, 'wasmMirror'), 'Unexpected upstream mirror on third');
  } else {
    const previous = new URL(existing.wasmMirror);
    assert(previous.origin === 'http://103.91.210.16:67' && previous.pathname === existing.assets['bundle.wasm.bin'],
      'Existing mirror origin or path changed');
    assert.equal(mirrorWasmSha256, assetHashes['bundle.wasm.bin'], 'Mirror can only serve identical packed WASM');
    next.wasmMirror = previous.origin + assets['bundle.wasm.bin'];
  }
  const encoded = Buffer.from(json(next)).toString('base64url');
  const eol = before.includes('\r\n') ? '\r\n' : '\n';
  let rendered = before;
  let previousRoot;
  for (const uri of ['/', '/index.html']) {
    const block = location(rendered, uri, '=');
    const root = block.match(/\broot (\/var\/lib\/ashflor-server-guide-layout-[0-9a-f]+|\/opt\/florrclone\/releases\/[a-zA-Z0-9_-]+);/);
    assert(root && (!previousRoot || previousRoot === root[1]) && block.includes(marker), 'Unexpected homepage layout or maintenance gate');
    assert.equal([...block.matchAll(tokenPattern)].length, 1, 'Missing homepage routing filter');
    previousRoot = root[1];
    const replacement = block.replace(root[0], 'root ' + release + ';')
      .replace(tokenPattern, "sub_filter 'FLIX_CLIENT_ASSET_ROUTING' '" + encoded + "';");
    rendered = replaceOnce(rendered, block, replacement, 'Ambiguous homepage block');
  }

  for (const name of names) {
    const oldUri = existing.assets[name];
    const oldBlock = location(rendered, oldUri, '=');
    const oldAlias = 'alias ' + old + '/' + name + ';';
    assert(oldBlock.includes(oldAlias), 'Prior active digest points at unexpected artifact: ' + name);
    if (assets[name] !== oldUri && !hasExact(rendered, assets[name])) {
      const clone = oldBlock.replace(oldUri, assets[name]).replace(oldAlias, 'alias ' + release + '/' + name + ';');
      rendered = replaceOnce(rendered, oldBlock, oldBlock + eol + eol + clone, 'Ambiguous active digest block');
    }

    // Keep existing digest paths and old version redirects byte-for-byte. Some returning clients
    // are still using a previously downloaded shell. Reload recovery selects the new shell.
    assert(!hasExact(rendered, assetBase+name), 'Candidate version route already exists');
    if (role === 'third') {
      const oldRedirect = location(rendered, oldBase+name, '=');
      const oldTarget = 'return 302 ' + oldUri + '$is_args$args;';
      assert(oldRedirect.includes(oldTarget), 'Unexpected version redirect: ' + name);
      const clone = oldRedirect.replace(oldBase+name,assetBase+name)
        .replace(oldTarget, 'return 302 ' + assets[name] + '$is_args$args;');
      rendered = replaceOnce(rendered, oldRedirect, oldRedirect + eol + eol + clone, 'Ambiguous version redirect');
    }
  }

  if (role === 'third') {
    const queueBlock = location(rendered, '/download-queue/', '^~');
    const body = queueBlock.match(/return 200 '({[^\r\n]*})';/);
    assert(body, 'Missing direct-download ticket response');
    const response = JSON.parse(body[1]);
    assert(response.ticket === 'direct-download' && response.assetBase.endsWith(oldBase),
      'Unexpected download queue settings');
    assert.deepEqual(response.assets, existing.assets, 'Download queue and shell routes disagree');
    const origin = response.assetBase.slice(0,-oldBase.length);
    response.assetBase = origin + assetBase;
    response.assets = assets;
    rendered = replaceOnce(rendered, queueBlock, queueBlock.replace(body[1], json(response)), 'Ambiguous download queue');
    rendered = replaceOnce(rendered, 'return 302 ' + origin + oldBase + '$1;',
      'return 302 ' + origin + assetBase + '$1;', 'Missing third legacy bundle redirect');
  }
  assert.deepEqual(readRouting(rendered), next);
  return rendered;
}

module.exports = { checkFrontendHtml, renderLiveFrontend, readRouting };
