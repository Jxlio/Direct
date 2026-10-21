// Build-time conversion only. The application has no JavaScript/SVG runtime.
const fs = require('fs');
const path = require('path');
const {Resvg} = require(path.resolve(__dirname, '../../../out/icon-tools/node_modules/@resvg/resvg-js'));
const names = ['message-circle', 'users', 'phone', 'phone-off', 'mic', 'mic-off', 'send', 'monitor', 'calendar-days', 'gamepad-2', 'settings'];
const colors = ['#182234', '#687489', '#3a67f4', '#ffffff'];
const root = path.join(__dirname, 'lucide');
fs.mkdirSync(path.join(root, 'generated'), {recursive:true});
for (let i=0;i<names.length;i++) {
  const svg = fs.readFileSync(path.join(root, names[i]+'.svg'), 'utf8');
  for (let c=0;c<colors.length;c++) {
    const rendered = new Resvg(svg.replaceAll('currentColor', colors[c]), {fitTo:{mode:'width',value:128}}).render();
    fs.writeFileSync(path.join(root, 'generated', `${i}-${c}.png`), rendered.asPng());
  }
}
