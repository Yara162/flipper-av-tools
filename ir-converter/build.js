// Inlines src/irconv.js into src/page.template.html and writes ir-converter.html
const fs = require('fs');
const path = require('path');
const src = (f) => fs.readFileSync(path.join(__dirname, 'src', f), 'utf8');
const html = src('page.template.html').replace('/*IRCONV*/', () => src('irconv.js'));
fs.writeFileSync(path.join(__dirname, 'ir-converter.html'), html);
console.log('wrote ir-converter.html');
