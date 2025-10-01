// ESP8266 (NodeMCU v3) — Вольтметр/Давление, буфер в браузере, Пуск/Пауза,
// вертикальный маркер с интерполяцией, верхняя LIVE-шкала,
// индикация P/V на D1/D2 + управление реле (D2=HIGH в режиме давления).
//
// Старт безопасный: V-режим (0–50 В), D1=HIGH, D2=LOW. Режим от тумблера D6 с неблокирующим дебаунсом.

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

ESP8266WebServer server(80);

// ===== Пины =====
#define MODE_PIN   D6       // вход P/V: HIGH = V (вольтметр), LOW = P (давление)
#define LED_V_PIN  D1       // GPIO5: индикация "V" (HIGH при V)
#define RELAY_P_PIN D2      // GPIO4: индикация "P" + реле (HIGH при P) — транзисторный ключ!

// ===== Железо АЦП / делители =====
#define ADC_RESOLUTION  1023.0f
#define ADC_REF_V       3.2f      // В на A0
#define SENSOR_SUPPLY_V 5.0f      // датчик давления 0..5 В
#define VOLT_DIVIDER_K  19.0f     // коэффициент делителя для 0..50 В на входе
#define VOLT_MAX_V      50.0f

// ===== EEPROM (диапазон давления) =====
const int EEPROM_SIZE   = 64;
const int EE_ADDR_MAGIC = 0;
const int EE_ADDR_RANGE = 4;
const uint32_t MAGIC    = 0xBADA55EE;

volatile float g_range_bar = 50.0f;  // 5/50/500/1000 (по умолчанию 50)

// ===== Дребезг D6 (неблокирующий) =====
const uint32_t DEBOUNCE_MS = 20;
volatile bool  g_mode_is_volt = true;   // текущее дебаунс-значение режима (true=V)
static bool    db_last_raw = true;
static bool    db_stable   = true;
static uint32_t db_last_change_ms = 0;

static inline float adcV(int raw){ return (raw/ADC_RESOLUTION)*ADC_REF_V; }
static inline float sensorV_toBar(float v, float range){ float bar=(v/SENSOR_SUPPLY_V)*range; return (bar<0)?0:bar; }

// Применить уровни на выходах (LED/реле) по режиму
static void applyPVOutputs(bool isVolt){
  digitalWrite(LED_V_PIN,   isVolt ? HIGH : LOW);
  digitalWrite(RELAY_P_PIN, isVolt ? LOW  : HIGH); // P=HIGH → реле шунтирует Radd
}

// Обновить дебаунс D6 и при смене режима — выходы
static void updatePVModeDebounced(){
  bool raw = (digitalRead(MODE_PIN) == HIGH); // HIGH=V
  uint32_t now = millis();
  if(raw != db_last_raw){
    db_last_raw = raw;
    db_last_change_ms = now;
  }else{
    if((now - db_last_change_ms) >= DEBOUNCE_MS){
      if(db_stable != raw){
        db_stable = raw;
        g_mode_is_volt = db_stable;
        applyPVOutputs(g_mode_is_volt);
      }
    }
  }
}

// ======= ВЕБ UI =======
const char HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="ru"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover,user-scalable=no">
<title>IVECO Scale</title>
<style>
  :root{ --blue:#3aa3ff; --blueDark:#2a7edc; --vh:100vh; --padX:20px; --padTop:14px; --footerBottom:16px; --footerPad:10px; --scaleH:92px; --digitH:64px; --titleGap:10px; }
  html,body{height:100%;margin:0;background:#fff;overflow:hidden;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif; touch-action:none; overscroll-behavior:none;}
  .stage{position:fixed; inset:0; height:calc(var(--vh) * 100); box-sizing:border-box;}
  .frame{position:absolute; inset:0; box-sizing:border-box; border:4px solid var(--blue); border-radius:10px; background:#fff;}
  .topArea{position:absolute; left:var(--padX); right:var(--padX); top:var(--padTop);}
  .titleWrap{text-align:center; line-height:1.05; user-select:none; pointer-events:none;}
  .brand{font-weight:800; color:#2a7edc; font-size:24px; letter-spacing:1px;}
  .made{color:#000; font-weight:700; font-size:14px;}
  .titleGap{height:var(--titleGap);}
  .scaleBox{height:var(--scaleH);} #scaleCanvas{width:100%; height:100%; display:block;}
  .digitalWrap{height:var(--digitH); margin-top:8px; background:rgba(246,249,255,.85); border:1px solid #b7d6ff; border-radius:10px; display:flex; align-items:center; justify-content:flex-start; box-shadow:0 2px 8px rgba(0,0,0,.06); cursor:pointer; user-select:none; gap:8px; padding:0 12px;}
  .playIcon{width:40px; display:flex; align-items:center; justify-content:center; font-weight:900; font-size:22px; color:#084f9c;}
  .digitalValue{color:#000; font-weight:900; font-family:ui-monospace, Menlo, Consolas, monospace; letter-spacing:.6px; font-size:42px; line-height:1; white-space:nowrap; margin-left:6px;}
  .digitalWrap:active{filter:brightness(.97);}
  .chartBox{position:absolute; left:var(--padX); right:var(--padX);} #chart{width:100%; height:100%; display:block;}
  .footer{position:absolute; left:var(--padX); right:var(--padX); bottom:var(--footerBottom); padding:var(--footerPad); background:#f6f9ff; border:1px solid #cae3ff; border-radius:12px;}
  .row{display:flex; gap:8px}
  .btn{flex:1 1 0; height:48px; border-radius:10px; border:1px solid #b7d6ff; background:#fff; color:#0b3c7a; font-size:16px; font-weight:700; text-align:center;}
  .btn.active{background:#e9f3ff; border-color:#7fb9ff; color:#084f9c;}
  .btn:disabled{opacity:.45}
  .modeBadge{position:absolute; top:6px; right:6px; font:12px/1 system-ui; color:#084f9c;}
</style>
</head>
<body>
  <div class="stage">
    <div class="frame"></div>

    <div class="topArea" id="topArea">
      <div class="titleWrap">
        <div class="brand">IVECO</div>
        <div class="made">made in CAYLA garage</div>
      </div>
      <div class="titleGap"></div>

      <div class="scaleBox"><canvas id="scaleCanvas"></canvas></div>

      <!-- Цифровой дисплей = кнопка Пуск/Пауза -->
      <div class="digitalWrap" id="digitalWrap" title="Нажмите для Пуск/Пауза">
        <div class="playIcon" id="playIcon">▶</div>
        <div class="digitalValue" id="digitalValue">0.00</div>
      </div>
    </div>

    <div class="chartBox" id="chartBox"><canvas id="chart"></canvas></div>
    <div class="modeBadge" id="modeBadge">вольтметр</div>

    <div class="footer" id="footer">
      <div class="row" id="rangeRow">
        <button class="btn" data-range="5">0–5 bar</button>
        <button class="btn" data-range="50">0–50 bar</button>
        <button class="btn" data-range="500">0–500 bar</button>
        <button class="btn" data-range="1000">0–1000 bar</button>
      </div>
    </div>
  </div>

<script>
/* ===== режим/диапазон ===== */
let MODE='v';            // на старте принудительно V
let yUnit='V';
let RANGE_MAX = 50;      // для давления (меняется кнопками)
let lastMode='v';

/* ===== верхняя шкала / LIVE ===== */
let topLiveValue = null;
let topFrozen    = null;

/* ===== Пуск/Пауза ===== */
let paused=false;        // ▶=false, ⏸=true

/* ===== маркер ===== */
let markerFrac=1.0, dragging=false;

/* ===== буфер и время ===== */
const SAMPLE_MS=200, MAX_BUF_MS=5*60*1000, VIEW_SEC_DEF=60;
let VIEW_SEC=VIEW_SEC_DEF;
const TSPAN_MS = ()=> VIEW_SEC*1000;   // ЕДИНЫЙ span времени

let buf=[], lastT=performance.now();
let sweepStart=performance.now(); // левая граница всегда тут
let freezeEnd=null;

function pressureScaleTicks(max){
  if(max===5)    return {max, major:1,   minor:0.5};
  if(max===50)   return {max, major:10,  minor:5};
  if(max===500)  return {max, major:100, minor:50};
  if(max===1000) return {max, major:100, minor:50};
  return {max, major:10, minor:5};
}
function voltageScaleTicks(){ return {max:50, major:1, minor:0.5}; }
function pressureChartTicks(max){ return pressureScaleTicks(max); }
function voltageChartTicks(){ return {max:50, major:10, minor:5}; }
function scaleTicks(){ return MODE==='v'?voltageScaleTicks():pressureScaleTicks(RANGE_MAX); }
function chartTicks(){ return MODE==='v'?voltageChartTicks():pressureChartTicks(RANGE_MAX); }

function setVH(){ const vv=window.visualViewport; const h=(vv && vv.height?vv.height:window.innerHeight); document.documentElement.style.setProperty('--vh',(h/100)+'px'); }
function layout(){
  setVH();
  const stageR=document.querySelector('.stage').getBoundingClientRect();
  const footerR=document.getElementById('footer').getBoundingClientRect();
  const topPad=14,bottomPad=16;
  const availTopH=stageR.height-(footerR.height+bottomPad)-topPad-12;
  const scaleH=parseInt(getComputedStyle(document.documentElement).getPropertyValue('--scaleH'))||92;
  const digitH=parseInt(getComputedStyle(document.documentElement).getPropertyValue('--digitH'))||64;
  const needH=(24+4+16)+(parseInt(getComputedStyle(document.documentElement).getPropertyValue('--titleGap'))||10)+scaleH+8+digitH;
  if(needH>availTopH){
    const over=needH-availTopH;
    const sShrink=Math.min(over, Math.max(20,scaleH-50));
    const dShrink=Math.max(0, over - sShrink);
    document.documentElement.style.setProperty('--scaleH', (scaleH - sShrink) + 'px');
    document.documentElement.style.setProperty('--digitH', (digitH - dShrink) + 'px');
  }
  const stageNow=document.querySelector('.stage').getBoundingClientRect();
  const footerNow=document.getElementById('footer').getBoundingClientRect();
  const topNow=document.getElementById('topArea').getBoundingClientRect();
  const chartBox=document.getElementById('chartBox');
  const topOffset=Math.round(topNow.bottom - stageNow.top)+10;
  const bottomOffset=Math.round(stageNow.bottom - footerNow.top)+10;
  chartBox.style.top=topOffset+'px';
  chartBox.style.bottom=bottomOffset+'px';
  drawTopScale(); drawChart();
}
addEventListener('resize', ()=>{ clearTimeout(window.__rto); window.__rto=setTimeout(layout,80); });
addEventListener('load', layout);

/* ===== верхняя шкала ===== */
const scaleCanvas=document.getElementById('scaleCanvas');
const sctx=scaleCanvas.getContext('2d');

function fitCanvas(canvas, ctx){ const dpr=window.devicePixelRatio||1; const r=canvas.getBoundingClientRect(); canvas.width=Math.round(r.width*dpr); canvas.height=Math.round(r.height*dpr); ctx.setTransform(dpr,0,0,dpr,0,0); }

function drawTopScale(){
  fitCanvas(scaleCanvas, sctx);
  const W=scaleCanvas.width/(window.devicePixelRatio||1), H=scaleCanvas.height/(window.devicePixelRatio||1);
  sctx.clearRect(0,0,W,H);

  const {max,major,minor}=scaleTicks();
  const color=getComputedStyle(document.documentElement).getPropertyValue('--blue').trim()||'#3aa3ff';
  const colorMinor='#9fd0ff', y=Math.round(H*0.35);

  // базовая
  sctx.beginPath(); sctx.moveTo(0.5,y+0.5); sctx.lineTo(W-0.5,y+0.5); sctx.lineWidth=1; sctx.strokeStyle=color; sctx.stroke();

  const X=v=>(v/max)*W; const isMult=(v,s)=>Math.abs(v/s - Math.round(v/s))<1e-9;

  // minor
  sctx.strokeStyle=colorMinor; sctx.lineWidth=1;
  for(let v=0; v<=max+1e-9; v+=minor){
    if(isMult(v,major)) continue;
    const x=Math.round(X(v))+0.5, len=Math.min(8,H*0.25);
    sctx.beginPath(); sctx.moveTo(x,y+0.5); sctx.lineTo(x,y+len+0.5); sctx.stroke();
  }

  // major + метки
  sctx.strokeStyle=color; sctx.lineWidth=1.5; sctx.fillStyle="#003a7a";
  const fs=Math.max(8, Math.floor(H*0.20)); const gap=Math.max(12, Math.floor(H*0.32)); sctx.font=fs+"px system-ui, sans-serif";
  for(let v=0; v<=max+1e-9; v+=major){
    const x=Math.round(X(v))+0.5; const L=(max<=5)?Math.min(20,H*0.5):Math.min(18,H*0.45);
    sctx.beginPath(); sctx.moveTo(x,y+0.5); sctx.lineTo(x,y+L+0.5); sctx.stroke();
    let drawLabel=true; if(MODE==='v') drawLabel=(Math.round(v)%5==0); if(!drawLabel) continue;
    let lbl; if(MODE==='p' && max===1000) lbl=(v==0?"0K":(v/1000).toFixed(1)+"K"); else lbl=String(v);
    sctx.fillText(lbl, x - sctx.measureText(lbl).width/2, y+L+gap);
  }

  // красный индикатор (бар) по базовой линии
  const showVal = paused ? topFrozen : topLiveValue;
  if(showVal!=null && isFinite(showVal)){
    const v=Math.max(0, Math.min(max, showVal)), x=X(v);
    sctx.save(); sctx.strokeStyle='#ff3b30'; sctx.lineCap='round'; sctx.lineWidth=6;
    sctx.beginPath(); sctx.moveTo(0.5, y+0.5); sctx.lineTo(x+0.5, y+0.5); sctx.stroke(); sctx.restore();
  }
}

/* ===== график ===== */
const chart=document.getElementById('chart'), cctx=chart.getContext('2d');
let chartLayout=null;

function drawChart(){
  fitCanvas(chart, cctx);
  const W=chart.width/(window.devicePixelRatio||1), H=chart.height/(window.devicePixelRatio||1);
  cctx.clearRect(0,0,W,H);

  const padL=52, padR=10, padT=10, padB=22;
  const x0=padL, x1=W-padR, y0=padT, y1=H-padB;

  const {max:YMAX, major:YMAJ, minor:YMIN}=chartTicks();

  cctx.strokeStyle='#cfe7ff'; cctx.lineWidth=1; cctx.strokeRect(x0,y0,x1-x0,y1-y0);

  const yFor=v=>y1 - (v/YMAX)*(y1-y0); const isMult=(v,s)=>Math.abs(v/s - Math.round(v/s))<1e-9;

  // сетка
  cctx.strokeStyle='#e6f2ff'; cctx.lineWidth=1;
  for(let v=0; v<=YMAX; v+=YMIN){ if(isMult(v,YMAJ)) continue; const y=Math.round(yFor(v))+0.5; cctx.beginPath(); cctx.moveTo(x0,y); cctx.lineTo(x1,y); cctx.stroke(); }
  cctx.strokeStyle='#b7d6ff'; cctx.fillStyle='#003a7a'; cctx.font='12px system-ui, sans-serif';
  for(let v=0; v<=YMAX; v+=YMAJ){ const y=Math.round(yFor(v))+0.5; cctx.beginPath(); cctx.moveTo(x0,y); cctx.lineTo(x1,y); cctx.stroke(); const lbl=String(v); cctx.fillText(lbl, x0-6-cctx.measureText(lbl).width, y+4); }

  const now=performance.now();
  let winStart=sweepStart;
  let winEnd = paused ? (freezeEnd||now) : now;
  const span = (VIEW_SEC*1000);
  if(winEnd > winStart + span) winEnd = winStart + span;

  const xForT = t => x0 + ((t - winStart)/span)*(x1 - x0);

  const pts = buf.filter(p=>p.t>=winStart && p.t<=winEnd);
  if(pts.length>=2){
    cctx.beginPath();
    pts.forEach((p,i)=>{ const x=xForT(p.t), y=yFor(Math.max(0,Math.min(YMAX,p.v))); if(i===0)cctx.moveTo(x,y); else cctx.lineTo(x,y); });
    cctx.lineWidth=2; cctx.strokeStyle='#ff3b30'; cctx.stroke();
  }

  // вертикальный маркер
  const mx = x0 + markerFrac*(x1-x0);
  cctx.save(); cctx.strokeStyle='#ff3b30'; cctx.lineWidth=2; cctx.beginPath(); cctx.moveTo(mx+0.5,y0); cctx.lineTo(mx+0.5,y1); cctx.stroke(); cctx.restore();

  // точка-пересечение
  if(pts.length>=2){
    const tTarget = winStart + markerFrac*span;
    let hit=null;
    for(let i=0;i<pts.length-1;i++){
      const t0=pts[i].t, t1=pts[i+1].t;
      if(tTarget>=t0 && tTarget<=t1){
        const v0=pts[i].v, v1=pts[i+1].v;
        const r=(tTarget-t0)/Math.max(1e-6,(t1-t0));
        const v=v0 + r*(v1-v0);
        hit={x:xForT(tTarget), y:yFor(Math.max(0,Math.min(YMAX,v))), v};
        break;
      }
    }
    if(hit){
      cctx.save(); cctx.fillStyle='#000'; cctx.beginPath(); cctx.arc(hit.x, hit.y, 4.5, 0, Math.PI*2); cctx.fill(); cctx.restore();
      cctx.fillStyle='#000'; cctx.font='bold 12px system-ui, sans-serif';
      const label=hit.v.toFixed(2)+' '+yUnit;
      const tx=Math.min(Math.max(hit.x+8, x0+4), x1-4);
      const ty=Math.max(y0+12, Math.min(y1-4, hit.y-8));
      cctx.fillText(label, tx, ty);
      if(paused){ setDisplay(hit.v, yUnit); }
    }
  }

  // подпись Y
  cctx.fillStyle='#003a7a'; cctx.font='12px system-ui, sans-serif';
  cctx.save(); cctx.translate(18,(y0+y1)/2); cctx.rotate(-Math.PI/2); cctx.textAlign='center'; cctx.fillText(yUnit,0,0); cctx.restore();
}

function setMarkerFromClientX(clientX){
  const r=chart.getBoundingClientRect();
  const padL=52, padR=10;
  const x0=padL, x1=r.width-padR;
  const x=clientX - r.left;
  markerFrac=Math.max(0, Math.min(1, (x - x0) / (x1 - x0)));
  drawChart();
}
chart.addEventListener('pointerdown', (e)=>{ chart.setPointerCapture(e.pointerId); setMarkerFromClientX(e.clientX); });
chart.addEventListener('pointermove', (e)=>{ if(e.pressure || (e.buttons&1)) setMarkerFromClientX(e.clientX); });
chart.addEventListener('pointerup',   (e)=>{ try{ chart.releasePointerCapture(e.pointerId);}catch(_){} });

/* ===== кнопки диапазона (только давление) ===== */
const rangeRow=document.getElementById('rangeRow');
function applyActive(){ [...rangeRow.querySelectorAll('.btn')].forEach(b=> b.classList.toggle('active', parseInt(b.dataset.range,10)===RANGE_MAX)); }
rangeRow.addEventListener('click',(e)=>{
  const btn=e.target.closest('.btn[data-range]'); if(!btn||MODE!=='p') return;
  const v=parseInt(btn.dataset.range,10); if(!isFinite(v)) return;
  RANGE_MAX=v; applyActive(); drawTopScale();
  if(!paused){ buf=[]; sweepStart=performance.now(); markerFrac=1.0; }
  drawChart();
});

/* ===== Пуск/Пауза по клику на дисплей ===== */
const digitalWrap=document.getElementById('digitalWrap');
const playIcon=document.getElementById('playIcon');
function updatePlayIcon(){ playIcon.textContent = paused ? '⏸' : '▶'; }
digitalWrap.addEventListener('click', ()=>{
  paused=!paused;
  if(paused){ freezeEnd=performance.now(); topFrozen=topLiveValue; }
  else{ buf=[]; sweepStart=performance.now(); freezeEnd=null; topFrozen=null; markerFrac=1.0; }
  updatePlayIcon(); drawTopScale(); drawChart();
});

/* ===== UI режима ===== */
const modeBadge=document.getElementById('modeBadge');
function applyModeUI(){
  yUnit = (MODE==='v'?'V':'bar');
  modeBadge.textContent = (MODE==='v'?'вольтметр':'давление');
  document.querySelectorAll('.btn[data-range]').forEach(b=> b.disabled = (MODE==='v'));
  drawTopScale(); drawChart();
}

/* ===== опрос режима/данных ===== */
async function pollMode(){
  try{ const r=await fetch('/mode',{cache:'no-store'}); if(!r.ok) throw 0; const t=(await r.text()).trim(); MODE=(t==='v')?'v':'p'; }
  catch(_){ MODE='v'; }
  if(MODE!==lastMode){
    lastMode=MODE;
    if(!paused){ buf=[]; sweepStart=performance.now(); markerFrac=1.0; }
    applyModeUI();
  }
}
async function pollData(){
  if(paused) return;
  const tNow=performance.now();
  if(tNow-lastT>=SAMPLE_MS){
    lastT=tNow;
    let v;
    try{ const r=await fetch('/data',{cache:'no-store'}); if(!r.ok) throw 0; const txt=await r.text(); v=parseFloat(txt); if(!isFinite(v)) throw 0; }
    catch(_){
      const t=tNow/1000;
      if(MODE==='v'){ v=20+5*Math.sin(t*0.7)+0.2*(Math.random()-0.5); v=Math.max(0,Math.min(50,v)); }
      else{ const M=RANGE_MAX; v=M*0.4+(M*0.1)*Math.sin(t*0.6)+(M*0.01)*(Math.random()-0.5); v=Math.max(0,Math.min(M,v)); }
    }
    buf.push({t:tNow, v});
    const tMin=tNow - MAX_BUF_MS; while(buf.length && buf[0].t<tMin) buf.shift();
    setDisplay(v, yUnit); topLiveValue=v;
    drawTopScale(); drawChart();
  }
}

/* ===== луп ===== */
(function loopers(){ function tick(){ pollMode(); pollData(); requestAnimationFrame(tick);} tick(); })();
(function init(){ MODE='v'; yUnit='V'; RANGE_MAX=50; applyActive(); layout(); setDisplay(0,yUnit); markerFrac=1.0; updatePlayIcon(); })();
</script>
</body></html>
)HTML";

// ============ HTTP ============
void handleRoot(){ server.send_P(200, PSTR("text/html"), HTML); }
void handleMode(){ server.send(200, "text/plain", g_mode_is_volt ? "v" : "p"); }
void handleGetRange(){ server.send(200, "text/plain", String(g_range_bar, 0)); }
void handleSetRange(){
  if(!server.hasArg("max")){ server.send(400,"text/plain","missing max"); return; }
  long v=server.arg("max").toInt();
  if(v==5||v==50||v==500||v==1000){ g_range_bar=(float)v; EEPROM.put(EE_ADDR_MAGIC, MAGIC); EEPROM.put(EE_ADDR_RANGE, g_range_bar); EEPROM.commit(); server.send(200,"text/plain","ok"); }
  else server.send(400,"text/plain","bad value");
}
void handleData(){
  int raw=analogRead(A0);
  float v_a0=adcV(raw);
  if(g_mode_is_volt){
    float volts=v_a0*VOLT_DIVIDER_K; if(volts<0) volts=0; if(volts>VOLT_MAX_V) volts=VOLT_MAX_V;
    server.send(200,"text/plain",String(volts,2));
  }else{
    float v_sensor=v_a0*(SENSOR_SUPPLY_V/ADC_REF_V);
    float bar=sensorV_toBar(v_sensor, g_range_bar);
    server.send(200,"text/plain",String(bar,(g_range_bar>=50.0f)?1:2));
  }
}

void setup(){
  Serial.begin(115200); delay(150);

  // Пины
  pinMode(MODE_PIN, INPUT_PULLUP);     // тумблер
  pinMode(LED_V_PIN, OUTPUT);
  pinMode(RELAY_P_PIN, OUTPUT);

  // Безопасный старт: V-режим 50 В
  g_mode_is_volt = true;
  digitalWrite(LED_V_PIN, HIGH);
  digitalWrite(RELAY_P_PIN, LOW);

  // EEPROM (диапазон давления)
  EEPROM.begin(EEPROM_SIZE);
  float saved=50;
  uint32_t m=0; EEPROM.get(EE_ADDR_MAGIC, m);
  if(m==MAGIC){
    EEPROM.get(EE_ADDR_RANGE, saved);
    if(saved==5||saved==50||saved==500||saved==1000) g_range_bar=saved;
  }

  // Wi-Fi AP
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF); delay(50);
  WiFi.forceSleepWake(); delay(100);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP("FRAME_AP", "", 6, false, 4);

  server.on("/", handleRoot);
  server.on("/mode", handleMode);
  server.on("/data", handleData);
  server.on("/range", handleGetRange);
  server.on("/setRange", handleSetRange);
  server.begin();

  Serial.println("AP: FRAME_AP  |  URL: http://192.168.4.1/");
}

void loop(){
  server.handleClient();
  // обновляем режим (без блокировок)
  updatePVModeDebounced();
  yield();
}
