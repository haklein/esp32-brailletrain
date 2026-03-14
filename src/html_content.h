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
#statpanel{display:none;background:#1a1a2e;border-top:1px solid #333;padding:12px 16px}
#statpanel.show{display:block}
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
#wnets .wn{padding:5px 8px;cursor:pointer;font-size:13px;border-radius:3px;display:flex;justify-content:space-between;align-items:center}
#wnets .wn:hover{background:#2a2a4e}
.wr{color:#666;font-size:11px}
.wi{display:block;background:#222;border:1px solid #444;color:#ccc;padding:5px 8px;border-radius:4px;font-size:13px;width:100%;margin-top:4px}
#spanel{font-size:12px;margin-top:8px}
#spanel table{border-collapse:collapse;width:100%}
#spanel th,#spanel td{padding:3px 6px;text-align:left;border-bottom:1px solid #333}
#spanel th{color:#888;font-size:11px;text-transform:uppercase}
.conf-tag{display:inline-block;background:#2a2a4e;padding:1px 5px;border-radius:3px;margin:1px;font-size:11px}
</style>
</head>
<body>
<header>
<div><span class="title">BrailleTrain</span></div>
<div class="stats"><span id="ci"></span><span id="bi" style="font-size:12px;margin-right:4px;opacity:0.4" title="BrailleWave">&#x28FF;</span>Lvl <span id="hl">1</span> | <span id="hi">0</span> items | <span id="ha">0</span>%</div>
<div><button id="sb" style="margin-left:4px" onclick="document.getElementById('statpanel').classList.toggle('show');if(document.getElementById('statpanel').classList.contains('show'))tx({t:'reqstats'})">Stats</button><button id="sb" style="margin-left:4px" onclick="document.getElementById('settings').classList.toggle('show')">Settings</button></div>
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
<div id="statpanel">
<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px">
<h3 style="font-size:12px;color:#666;text-transform:uppercase;letter-spacing:.05em;margin:0">Statistics</h3>
<button class="btn" onclick="if(confirm('Reset ALL statistics and settings?'))tx({t:'resetstats'})">Reset all</button>
</div>
<div id="spanel"></div>
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
<label class="tg"><input type="checkbox" id="oa" onchange="audioOn=this.checked;localStorage.setItem('sound',audioOn?'1':'');if(audioOn)aInit()"> Sound feedback</label>
<label class="tg"><input type="checkbox" id="oe" onchange="sO('ergonomic',this.checked)"> Ergonomic positioning</label>
<label class="tg"><input type="checkbox" id="ok" checked onchange="sO('keepalive',this.checked)"> Auto-reconnect</label>
<label class="tg">Max word length <select id="owl" onchange="tx({t:'wordlen',v:parseInt(this.value)})" style="background:#222;border:1px solid #444;color:#ccc;padding:2px 6px;border-radius:4px;font-size:13px"><option value="0">no limit</option><option value="3">3</option><option value="4">4</option><option value="5">5</option><option value="6">6</option><option value="7">7</option><option value="8">8</option></select></label>
</div>
<div class="sg"><h3>Maintenance</h3>
<div class="bg">
<button class="btn" onclick="tx({t:'exercise',mins:5,ms:1000})">Exercise 5m</button>
<button class="btn" onclick="tx({t:'exercise',mins:15,ms:1000})">Exercise 15m</button>
<button class="btn" onclick="tx({t:'exercise',mins:5,ms:200})">Fast 5m</button>
<button class="btn" onclick="tx({t:'exercise',mins:15,ms:200})">Fast 15m</button>
<button class="btn" onclick="tx({t:'test'})">Test mode</button>
<button class="btn" onclick="tx({t:'reconnect'})">Reconnect</button>
<button class="btn" id="mstop" style="display:none" onclick="tx({t:'stop'})">Stop</button>
</div>
<div id="mstat" style="font-size:12px;color:#888;margin-top:4px"></div>
<div id="tkeys" style="font-size:12px;color:#aaa;margin-top:4px;max-height:120px;overflow-y:auto"></div>
</div>
<div class="sg"><h3>Device</h3>
<div class="bg">
<button class="btn" onclick="tx({t:'probe'})">Probe</button>
<button class="btn" onclick="tx({t:'syncrtc'})">Sync clock</button>
</div>
<div id="devinfo" style="font-size:12px;color:#888;margin-top:6px"></div>
<div id="dfirm" style="display:none;margin-top:6px">
<label class="tg" style="font-size:12px">Firmness <select id="firmsel" onchange="tx({t:'setfirm',v:parseInt(this.value)})" style="background:#222;border:1px solid #444;color:#ccc;padding:2px 6px;border-radius:4px;font-size:12px">
<option value="0">Soft</option><option value="1" selected>Medium</option><option value="2">Hard</option>
</select></label>
</div>
</div>
<div class="sg"><h3>WiFi</h3>
<div id="wst" style="font-size:12px;color:#888;margin-bottom:6px">Not connected</div>
<button class="btn" onclick="wScan()">Scan networks</button>
<div id="wnets" style="margin-top:6px"></div>
<div id="wpf" style="display:none;margin-top:6px">
<input class="wi" id="wssid" type="text" placeholder="Network" readonly>
<input class="wi" id="wpass" type="password" placeholder="Password">
<div style="margin-top:4px"><button class="btn" onclick="wConn()">Connect</button></div>
</div>
<button class="btn" id="wdis" style="display:none;margin-top:6px" onclick="wDisc()">Disconnect</button>
</div>
</div>
<script>
const T='eaioshbcdfgjtnrlkmpquyvxzw';
let ws,lv=1,wkl=null,lastAct=0,audioOn=false,actx=null,wasConnected=false,lastMsg=0;
function aInit(){if(!actx)actx=new(window.AudioContext||window.webkitAudioContext)();if(actx.state==='suspended')actx.resume()}
function aPlay(freq,type,dur,freq2){if(!audioOn||!actx)return;let o=actx.createOscillator(),g=actx.createGain();o.type=type;o.frequency.value=freq;g.gain.value=0.15;o.connect(g);g.connect(actx.destination);o.start();g.gain.exponentialRampToValueAtTime(0.001,actx.currentTime+dur);if(freq2){o.frequency.setValueAtTime(freq2,actx.currentTime+dur*0.4)}o.stop(actx.currentTime+dur)}
function aOK(){aInit();aPlay(880,'sine',0.12);setTimeout(()=>aPlay(1320,'sine',0.15),80)}
function aErr(){aInit();aPlay(150,'square',0.25,110)}
function wkPing(){lastAct=Date.now()}
setInterval(()=>{if(ws&&ws.readyState===1&&lastMsg&&Date.now()-lastMsg>12000){ws.close()}},5000);
function iG(){let g=document.getElementById('lgrid');for(let i=1;i<=26;i++){let b=document.createElement('div');b.className='lb'+(i<=1?' a':'');b.textContent=T[i-1].toUpperCase();b.dataset.l=i;b.onclick=()=>tx({t:'level',l:i});g.appendChild(b)}}
function sM(m){document.querySelectorAll('[data-m]').forEach(b=>b.classList.toggle('a',b.dataset.m===m));tx({t:'mode',m:m})}
function sO(k,v){tx({t:'opt',k:k,v:v})}
function rC(el,byte){el.innerHTML='';[[1,4],[2,5],[3,6]].forEach(([l,r])=>{[l,r].forEach(d=>{let o=document.createElement('div');o.className='dot '+((byte&(1<<(d-1)))?'on':'off');el.appendChild(o)})})}
function dT(b){let d=[];for(let i=1;i<=6;i++)if(b&(1<<(i-1)))d.push(i);return d.length===0?'empty':(d.length===1?'dot ':'dots ')+d.join(',')}
function uL(l,c){lv=l;document.getElementById('hl').textContent=l;document.querySelectorAll('.lb').forEach(b=>b.classList.toggle('a',parseInt(b.dataset.l)<=l));if(c)document.getElementById('ldisp').textContent=c.split('').map(x=>x.toUpperCase()).join(' ')}
function sP(s,w){let d=document.getElementById('display');d.textContent=w?'_'.repeat(s.length):'?';d.className=w?'wm':'';document.getElementById('error-detail').classList.remove('show');document.getElementById('word-detail').classList.remove('show')}
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
function wScan(){document.getElementById('wnets').innerHTML='<div style="color:#666;font-size:13px">Scanning...</div>';tx({t:'wscan'})}
function wConn(){let s=document.getElementById('wssid').value,p=document.getElementById('wpass').value;if(s)tx({t:'wconn',s:s,p:p})}
function wDisc(){tx({t:'wdisc'})}
function wUpd(m){let el=document.getElementById('wst'),db=document.getElementById('wdis');if(m&&m.s==='connected'){el.innerHTML='Connected to <b>'+m.ssid+'</b><br>IP: '+m.ip;db.style.display='inline-block'}else if(m&&m.wssid){el.innerHTML='Connected to <b>'+m.wssid+'</b><br>IP: '+m.wip;db.style.display='inline-block'}else{el.textContent='Not connected';db.style.display='none'}}
function conn(){
ws=new WebSocket('ws://'+location.host+'/ws');
ws.onopen=()=>{document.getElementById('ci').className='ok';lastMsg=Date.now();if(wasConnected){location.reload()}wasConnected=true};
ws.onclose=()=>{document.getElementById('ci').className='';setTimeout(conn,2000)};
ws.onmessage=e=>{lastMsg=Date.now();let m=JSON.parse(e.data);if(m.t==='hb')return;if(m.t==='prompt'||m.t==='ok'||m.t==='no'||m.t==='wp')wkPing();switch(m.t){
case'prompt':sP(m.s,m.w);break;
case'ok':sOK(m.s,m.w);aOK();break;
case'no':sNO(m);aErr();break;
case'level':uL(m.l,m.c);break;
case'stats':document.getElementById('hi').textContent=m.n;document.getElementById('ha').textContent=m.a;break;
case'advance':uL(m.l,m.c);sA(m);break;
case'wp':{let d=document.getElementById('display');d.className='wm';d.innerHTML='';let tgt=m.w,typed=m.p||'';for(let i=0;i<tgt.length;i++){let sp=document.createElement('span');if(i<typed.length){sp.textContent=typed[i].toUpperCase();sp.style.color='#aaa'}else if(i===typed.length){sp.textContent='_';sp.style.color='#fff';sp.style.opacity='.6'}else{sp.textContent='_';sp.style.color='#333'}d.appendChild(sp)}break}
case'state':uL(m.l,m.c);
if(m.mode){let v=m.mode;document.querySelectorAll('[data-m]').forEach(b=>b.classList.toggle('a',b.dataset.m===v))}
document.getElementById('om').checked=!!m.mirror;document.getElementById('os').checked=!!m.spacing;document.getElementById('oe').checked=!!m.ergonomic;document.getElementById('ok').checked=m.keepalive!==false;
document.getElementById('owl').value=m.wordlen||0;
document.getElementById('bi').style.opacity=m.brl?'1':'0.3';
document.getElementById('hi').textContent=m.n||0;document.getElementById('ha').textContent=m.a||0;wUpd(m);break;
case'wscanr':{let c=document.getElementById('wnets');c.innerHTML='';let ns=m.nets||[];if(ns.length===0){c.innerHTML='<div style="color:#666;font-size:13px">No networks found</div>';break}ns.forEach(n=>{let d=document.createElement('div');d.className='wn';let nm=document.createElement('span');nm.textContent=n.s;d.appendChild(nm);let info=document.createElement('span');info.className='wr';info.textContent=(n.e?'secured ':'open ')+n.r+'dBm';d.appendChild(info);d.onclick=()=>{document.getElementById('wssid').value=n.s;document.getElementById('wpf').style.display='block'};c.appendChild(d)});break}
case'wifi':wUpd(m);break;
case'brl':document.getElementById('bi').style.opacity=m.s?'1':'0.3';break;
case'devinfo':{let h='',el=document.getElementById('devinfo');
if(m.serial)h+='Serial: '+m.serial+'<br>';
if(m.firmware)h+='Firmware: '+m.firmware+'<br>';
if(m.cells)h+='Cells: '+m.cells+'<br>';
if(m.rtc)h+='Clock: '+m.rtc+'<br>';
h+='Ping: '+(m.ping?'OK':'no response')+'<br>';
if(!m.serial&&!m.firmware&&!m.cells&&!m.rtc)h+='<span style="color:#666">No extended protocol support detected (basic model)</span><br>';
el.innerHTML=h;
if(m.has_firmness){document.getElementById('dfirm').style.display='block';document.getElementById('firmsel').value=m.firmness||1}
else{document.getElementById('dfirm').style.display='none'}
break}
case'statsreset':document.getElementById('spanel').innerHTML='<div style="color:#888">Statistics reset.</div>';break;
case'fullstats':{let p=document.getElementById('spanel'),h='';
let acc=m.total?Math.round(m.correct*100/m.total):0;
h+='<div style="margin:6px 0;color:#ccc">Level '+m.level+' &middot; '+m.total+' items &middot; '+acc+'% accuracy</div>';
// Sort letters by accuracy (worst first)
let ls=(m.letters||[]).slice().sort((a,b)=>(a.n?a.c/a.n:1)-(b.n?b.c/b.n:1));
h+='<table><tr><th>Letter</th><th>Seen</th><th>Acc</th><th>Confused with</th></tr>';
ls.forEach(l=>{let a=l.n?Math.round(l.c*100/l.n):0;let col=a>=85?'#00ff88':a>=60?'#ffaa00':'#ff4444';
let cf=(l.cf||[]).sort((a,b)=>b.n-a.n).map(c=>'<span class="conf-tag">'+c.w.toUpperCase()+':'+c.n+'</span>').join('');
h+='<tr><td style="font-weight:bold;font-size:14px">'+l.l.toUpperCase()+'</td><td>'+l.n+'</td><td style="color:'+col+'">'+a+'%</td><td>'+cf+'</td></tr>'});
h+='</table>';
// Top confusion pairs across all letters
let pairs={};(m.letters||[]).forEach(l=>{(l.cf||[]).forEach(c=>{let k=[l.l,c.w].sort().join('');if(!pairs[k])pairs[k]={a:l.l,b:c.w,n:0};pairs[k].n+=c.n})});
let top=Object.values(pairs).sort((a,b)=>b.n-a.n).slice(0,10);
if(top.length){h+='<div style="margin-top:8px;color:#888">Top confusions:</div><div>';top.forEach(p=>{h+='<span class="conf-tag" style="font-size:12px">'+p.a.toUpperCase()+' / '+p.b.toUpperCase()+': '+p.n+'</span> '});h+='</div>'}
p.innerHTML=h;break}
case'extick':{let min=Math.floor(m.s/60),sec=m.s%60;document.getElementById('mstat').textContent='Exercise: '+min+':'+(sec<10?'0':'')+sec;document.getElementById('mstop').style.display='inline-block';break}
case'exdone':document.getElementById('mstat').textContent='Exercise complete';document.getElementById('mstop').style.display='none';break;
case'tdot':document.getElementById('mstat').textContent='Cell '+(m.c+1)+' dot '+m.d;document.getElementById('mstop').style.display='inline-block';break;
case'tkey':{let el=document.getElementById('tkeys');let s=document.createElement('span');s.textContent=m.k+(m.r?' \u2191':' \u2193')+' ';s.style.color=m.r?'#666':'#0f0';el.appendChild(s);el.scrollTop=el.scrollHeight;break}
}}}
function tx(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o))}
iG();conn();if(localStorage.getItem('sound')){audioOn=true;document.getElementById('oa').checked=true}
if('wakeLock' in navigator)setInterval(()=>{let want=lastAct>0&&Date.now()-lastAct<300000&&document.visibilityState==='visible';if(want&&!wkl)navigator.wakeLock.request('screen').then(s=>{wkl=s;s.onrelease=()=>{wkl=null}}).catch(()=>{});if(!want&&wkl)try{wkl.release()}catch(e){}},10000)
</script>
</body>
</html>)rawliteral";
