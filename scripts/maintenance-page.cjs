'use strict';
const fs = require('node:fs');
const assert = require('node:assert/strict');

const escape = value => String(value).replace(/[&<>"']/g, character =>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[character]);

function renderMaintenance(template, notes) {
  assert.equal(typeof notes.title, 'string');
  assert(Array.isArray(notes.changes) && notes.changes.length, 'Release notes are required');
  const sections = notes.sections || [{title: '', items: notes.changes}];
  assert.deepEqual(sections.flatMap(section => section.items), notes.changes);
  const groups = sections.map(({title, items}) =>
    (title ? '<h3>' + escape(title) + '</h3>' : '') +
    '<ul>' + items.map(item => '<li>' + escape(item) + '</li>').join('') + '</ul>').join('');
  const content = '<section id="maintenance-changes" aria-labelledby="maintenance-changes-title">' +
    '<div class="maintenance-notes-heading"><h2 id="maintenance-changes-title">' + escape(notes.title) +
    '</h2><a href="#game">跳到等待小游戏</a></div>' + groups + '</section>';
  assert(template.includes('<!-- RELEASE_NOTES -->'), 'Maintenance template is missing its release notes placeholder');
  return template.replace('<!-- RELEASE_NOTES -->', () => content);
}

module.exports = {renderMaintenance, escape};

if (require.main === module) {
  const [template, notes, result] = process.argv.slice(2);
  fs.writeFileSync(result, renderMaintenance(fs.readFileSync(template, 'utf8'), JSON.parse(fs.readFileSync(notes, 'utf8'))));
}
