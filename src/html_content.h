/*
 * Web UI HTML — served from PROGMEM.
 */
#pragma once

const char HTML_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BrailleTrain</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0a0a0a;color:#e0e0e0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;min-height:100vh;display:flex;flex-direction:column}
header{display:flex;justify-content:space-between;align-items:center;padding:8px 16px;background:#1a1a2e;border-bottom:1px solid #333;font-size:14px;flex-wrap:wrap;gap:4px}
header .title{font-weight:bold;font-size:16px}
header .stats{color:#aaa}
#main{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:16px;min-height:50vh}
#display{font-size:min(30vw,40vh);font-weight:700;letter-spacing:.05em;text-transform:uppercase;text-align:center;transition:color .2s;color:#fff;line-height:1.1}
#display.ok{color:#00ff88}
#display.no{color:#ff4444}
#display.wm{font-size:min(15vw,25vh);letter-spacing:.15em}
#error-detail{display:none;text-align:center;margin-top:16px;width:100%}
#error-detail.show{display:block}
.comparison{display:flex;justify-content:center;gap:30px;margin:12px 0;flex-wrap:wrap}
.comp-item{text-align:center}
.comp-item .label{font-size:13px;color:#888;margin-bottom:6px}
.comp-item .letter{font-size:min(10vw,90px);font-weight:bold;margin-top:4px}
.cell{display:inline-grid;grid-template-columns:1fr 1fr;grid-template-rows:1fr 1fr 1fr;gap:8px;padding:14px;background:#1a1a1a;border-radius:12px;border:2px solid #333}
.dot{width:min(6vw,36px);height:min(6vw,36px);border-radius:50%}
.dot.on{background:#fff}
.dot.off{background:#2a2a2a}
.dots-text{font-size:13px;color:#999;margin-top:6px}
#word-detail{display:none;margin-top:12px;font-size:min(8vw,60px);font-weight:bold;letter-spacing:.1em;text-align:center}
#word-detail.show{display:block}
#settings{display:none;background:#1a1a2e;border-top:1px solid #333;padding:12px 16px}
#settings.show{display:flex;flex-wrap:wrap;gap:16px}
.sg{min-width:140px}
.sg h3{font-size:12px;color:#666;margin-bottom:6px;text-transform:uppercase;letter-spacing:.05em}
.bg{display:flex;flex-wrap:wrap;gap:4px}
.btn{padding:5px 10px;border:1px solid #444;background:#222;color:#ccc;border-radius:4px;cursor:pointer;font-size:13px}
.btn.a{background:#3a3a8e;border-color:#6666cc;color:#fff}
.btn:hover{background:#333}
.lg{display:grid;grid-template-columns:repeat(13,1fr);gap:3px}
.lb{padding:3px;text-align:center;font-size:11px;border:1px solid #444;background:#222;color:#ccc;border-radius:3px;cursor:pointer}
.lb.a{background:#3a3a8e;border-color:#6666cc;color:#fff}
.lb:hover{background:#333}
#ldisp{font-size:11px;color:#555;margin-top:4px}
.tg{display:flex;align-items:center;gap:6px;cursor:pointer;font-size:13px;margin-bottom:4px}
.tg input{width:16px;height:16px}
#sb{cursor:pointer;padding:4px 12px;background:#2a2a4e;border:1px solid #444;color:#ccc;border-radius:4px;font-size:13px}
#ci{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:4px;background:#ff4444}
#ci.ok{background:#00ff88}
</style>
</head>
<body>
<header>
<div><span class="title">BrailleTrain</span></div>
<div class="stats"><span id="ci"></span>Lvl <span id="hl">1</span> | <span id="hi">0</span> items | <span id="ha">0</span>%</div>
<button id="sb" onclick="document.getElementById('settings').classList.toggle('show')">Settings</button>
</header>
<div id="main">
<div id="display">&mdash;</div>
<div id="error-detail">
<div class="comparison">
<div class="comp-item"><div class="label">Your input</div><div class="cell" id="gc"></div><div class="dots-text" id="gd"></div><div class="letter" id="gl" style="color:#ff4444"></div></div>
<div class="comp-item"><div class="label">Expected</div><div class="cell" id="ec"></div><div class="dots-text" id="ed"></div><div class="letter" id="el" style="color:#00ff88"></div></div>
</div>
</div>
<div id="word-detail"></div>
</div>
<div id="settings">
<div class="sg"><h3>Level</h3><div class="lg" id="lgrid"></div><div id="ldisp"></div></div>
<div class="sg"><h3>Mode</h3><div class="bg">
<button class="btn a" data-m="letters" onclick="sM('letters')">Letters</button>
<button class="btn" data-m="words" onclick="sM('words')">Words</button>
<button class="btn" data-m="mixed" onclick="sM('mixed')">Mixed</button>
</div></div>
<div class="sg"><h3>Options</h3>
<label class="tg"><input type="checkbox" id="om" onchange="sO('mirror',this.checked)"> Mirror (right hand)</label>
<label class="tg"><input type="checkbox" id="os" onchange="sO('spacing',this.checked)"> Wide word spacing</label>
</div>
</div>
<script>
const T='eaioshbcdfgjtnrlkmpquyvxzw';
let ws,lv=1;
function iG(){let g=document.getElementById('lgrid');for(let i=1;i<=26;i++){let b=document.createElement('div');b.className='lb'+(i<=1?' a':'');b.textContent=T[i-1].toUpperCase();b.dataset.l=i;b.onclick=()=>tx({t:'level',l:i});g.appendChild(b)}}
function sM(m){document.querySelectorAll('[data-m]').forEach(b=>b.classList.toggle('a',b.dataset.m===m));tx({t:'mode',m:m})}
function sO(k,v){tx({t:'opt',k:k,v:v})}
function rC(el,byte){el.innerHTML='';[[1,4],[2,5],[3,6]].forEach(([l,r])=>{[l,r].forEach(d=>{let o=document.createElement('div');o.className='dot '+((byte&(1<<(d-1)))?'on':'off');el.appendChild(o)})})}
function dT(b){let d=[];for(let i=1;i<=6;i++)if(b&(1<<(i-1)))d.push(i);return d.length===0?'empty':(d.length===1?'dot ':'dots ')+d.join(',')}
function uL(l,c){lv=l;document.getElementById('hl').textContent=l;document.querySelectorAll('.lb').forEach(b=>b.classList.toggle('a',parseInt(b.dataset.l)<=l));if(c)document.getElementById('ldisp').textContent=c.split('').map(x=>x.toUpperCase()).join(' ')}
function sP(s,w){let d=document.getElementById('display');d.textContent=s.toUpperCase();d.className=w?'wm':'';document.getElementById('error-detail').classList.remove('show');document.getElementById('word-detail').classList.remove('show')}
function sOK(s,w){let d=document.getElementById('display');d.textContent=s.toUpperCase();d.className='ok'+(w?' wm':'');document.getElementById('error-detail').classList.remove('show');document.getElementById('word-detail').classList.remove('show')}
function sNO(m){
let d=document.getElementById('display');
let wd=document.getElementById('word-detail');
let ed=document.getElementById('error-detail');
if(m.w){
d.innerHTML='';d.className='wm';
let exp=m.s,got=m.g||'',pc=m.pc||[];
for(let i=0;i<exp.length;i++){let sp=document.createElement('span');sp.textContent=exp[i].toUpperCase();sp.style.color=(pc[i])?'#00ff88':'#ff4444';d.appendChild(sp)}
wd.innerHTML='<div style="color:#888;font-size:16px;margin-bottom:4px">You typed:</div>';
for(let i=0;i<Math.max(exp.length,got.length);i++){let sp=document.createElement('span');sp.textContent=(got[i]||'_').toUpperCase();sp.style.color=(pc[i])?'#00ff88':'#ff4444';wd.appendChild(sp)}
wd.classList.add('show');ed.classList.remove('show')
}else{
d.textContent=m.s.toUpperCase();d.className='no';
rC(document.getElementById('gc'),m.gd);rC(document.getElementById('ec'),m.ed);
document.getElementById('gd').textContent=dT(m.gd);document.getElementById('ed').textContent=dT(m.ed);
document.getElementById('gl').textContent=(m.g||'?').toUpperCase();document.getElementById('el').textContent=m.s.toUpperCase();
ed.classList.add('show');wd.classList.remove('show')
}}
function sA(m){let d=document.getElementById('display');d.textContent=m.m;d.className='ok';d.style.fontSize='min(5vw,40px)';setTimeout(()=>d.style.fontSize='',4000)}
function conn(){
ws=new WebSocket('ws://'+location.host+'/ws');
ws.onopen=()=>document.getElementById('ci').className='ok';
ws.onclose=()=>{document.getElementById('ci').className='';setTimeout(conn,2000)};
ws.onmessage=e=>{let m=JSON.parse(e.data);switch(m.t){
case'prompt':sP(m.s,m.w);break;
case'ok':sOK(m.s,m.w);break;
case'no':sNO(m);break;
case'level':uL(m.l,m.c);break;
case'stats':document.getElementById('hi').textContent=m.n;document.getElementById('ha').textContent=m.a;break;
case'advance':uL(m.l,m.c);sA(m);break;
case'wp':{let d=document.getElementById('display');d.className='wm';d.innerHTML='';let tgt=m.w,typed=m.p||'';for(let i=0;i<tgt.length;i++){let sp=document.createElement('span');if(i<typed.length){sp.textContent=typed[i].toUpperCase();sp.style.color='#aaa'}else if(i===typed.length){sp.textContent='_';sp.style.color='#fff';sp.style.opacity='.6'}else{sp.textContent=tgt[i].toUpperCase();sp.style.color='#333'}d.appendChild(sp)}break}
case'state':uL(m.l,m.c);
if(m.mode){let v=m.mode;document.querySelectorAll('[data-m]').forEach(b=>b.classList.toggle('a',b.dataset.m===v))}
document.getElementById('om').checked=!!m.mirror;document.getElementById('os').checked=!!m.spacing;
document.getElementById('hi').textContent=m.n||0;document.getElementById('ha').textContent=m.a||0;break;
}}}
function tx(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o))}
iG();conn();
</script>
</body>
</html>)rawliteral";
