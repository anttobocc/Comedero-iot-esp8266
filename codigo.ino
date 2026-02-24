/**************** Comedero Automatico ESP8266 v2.4.1 ****************/
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Servo.h>
#include <time.h>
#include <ESP8266HTTPClient.h>

/* ================= WIFI ================= */
const char* ssid     = "SM de Antonella";
const char* password = "44572878";

/* ================= SERVO ================ */
Servo servo;
const int SERVO_PIN = D2;
int servoClosed = 0;     // 0..180
int servoOpenBase = 90;  // 90 por defecto

/* ================ WEB/NTP =============== */
ESP8266WebServer server(80);
const long gmtOffset_sec = -3 * 3600;
bool timeReady=false;

/* ============== MODELO / TIPOS ========== */
enum FoodType  : uint8_t { FT_CRIADORES=0, FT_PUP_SMALL=1 };
enum DogType   : uint8_t { DT_PUP=0, DT_ADULT_BIG=1, DT_ADULT_SMALL=2 };
enum Activity  : uint8_t { ACT_MOD=0, ACT_INT=1 };
/* Quitado PUP_12P */
enum PupAge    : uint8_t { PUP_2M=0, PUP_3_6=1, PUP_6_12=2, PUP_DST_3=3, PUP_5_8=4, PUP_8_12=5 };

/* --------- TABLAS Y ESTRUCTURAS --------- */
/* Cachorros (mordida pequeña) — SIN “12+ meses” */
struct PupRow { float maxKg; uint16_t g2m, g3_5, g5_8, g8_12; };
static const PupRow PUP_TAB[] = {
  { 3.0f,  30,  35,  80, 115 },
  { 5.0f,  55,  55,  80, 115 },
  { 8.0f,  75,  75,  90, 125 },
  {10.0f, 100, 100, 110, 155 }
};
static const uint8_t PUP_TAB_N = sizeof(PUP_TAB)/sizeof(PupRow);

/* ===== Sieger Criadores (adultos y cachorros) ===== */
static const float  CRI_W[6]     = { 5, 10, 20, 30, 40, 50 };
static const uint16_t CRI_AD_MOD[6] = {170,230,320,400,500,560};
static const uint16_t CRI_AD_INT[6] = {220,280,370,450,550,600};
static const uint16_t CRI_P_2M[6]   = { 85,150,250,340,400,450};
static const uint16_t CRI_P_3_6[6]  = {120,200,320,400,500,550};
static const uint16_t CRI_P_6_12[6] = {180,260,360,450,550,600};

/* ===== Resultado de calculo ===== */
struct CalcOut { uint16_t gDay, gPortion; uint8_t portions, angle; uint16_t deci; };

struct Settings {
  uint8_t  foodType   = FT_CRIADORES;
  uint8_t  dogType    = DT_ADULT_BIG;
  uint8_t  activity   = ACT_MOD;
  uint8_t  pupAge     = PUP_3_6;
  uint16_t weight10   = 50;   // 5.0 kg
  uint8_t f1h=8,f1m=0,f2h=12,f2m=0,f3h=18,f3m=0,f4h=21,f4m=30;
  uint8_t  suggAngle  = 90;
  uint16_t suggDeci   = 10;
} settings;

/* ============ TASAS g/seg (medidas) ====== */
static const float RATE_90  = 40.0f;
static const float RATE_100 = 60.0f;
static const float RATE_180 = 120.0f;



/* ======= THINGSPEAK (mínimo – Cachorros) ======= */
/* Channel: usá tu WRITE API KEY aquí */
const char* TS_WRITE_KEY = "6HS9PP4ZVDQGQB1W";   // <-- reemplazar
const char* TS_URL       = "http://api.thingspeak.com/update";

/* Respetamos 15 s entre updates por límites de ThingSpeak */
static unsigned long tsLastMs = 0;
static const unsigned long TS_MIN_INTERVAL_MS = 15000;

/* Enviamos:
   field1 = g_dispensed (gramos servidos)
   field2 = portion_no   (1..4; 0 si no aplica)
   field3 = g_per_portion_target (plan)
   field4 = g_day_plan (plan diario)
*/
bool tsUpdate(int g_dispensed, int portion_no, int g_per_portion, int g_day_plan) {
  unsigned long now = millis();
  if (now - tsLastMs < TS_MIN_INTERVAL_MS) return false;  // rate limit suave

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, TS_URL)) return false;

  String body = "api_key=" + String(TS_WRITE_KEY);
  if (g_dispensed      >= 0) body += "&field1=" + String(g_dispensed);
  if (portion_no       >= 0) body += "&field2=" + String(portion_no);
  if (g_per_portion    >= 0) body += "&field3=" + String(g_per_portion);
  if (g_day_plan       >= 0) body += "&field4=" + String(g_day_plan);

  http.addHeader("Content-Type","application/x-www-form-urlencoded");
  int code = http.POST(body);
  http.end();

  if (code == 200) tsLastMs = now;
  return (code == 200);
}

/* ============ HELPERS DE CALCULO ========= */
static inline float lerp(float a,float b,float t){ return a + (b-a)*t; }

static inline uint16_t interpCri(const float *X, const uint16_t *Y, uint8_t n, float kg){
  if(kg <= X[0]) return Y[0];
  if(kg >= X[n-1]) return Y[n-1];
  for(uint8_t i=0;i<n-1;i++){
    if(kg >= X[i] && kg <= X[i+1]){
      float t = (kg - X[i]) / (X[i+1] - X[i]);
      return (uint16_t)roundf( lerp((float)Y[i], (float)Y[i+1], t) );
    }
  }
  return Y[n-1];
}

static inline uint16_t criAdultOver50Extra(float kg, Activity act){
  if(kg <= 50.0f) return 0;
  float extraKg = kg - 50.0f;
  float steps = extraKg / 5.0f;
  uint16_t base = (act==ACT_INT)?CRI_AD_INT[5]:CRI_AD_MOD[5];
  return base + (uint16_t)roundf(steps * 50.0f);
}

static inline const PupRow& rowForKg(float kg){
  for(uint8_t i=0;i<PUP_TAB_N;i++) if(kg<=PUP_TAB[i].maxKg) return PUP_TAB[i];
  return PUP_TAB[PUP_TAB_N-1];
}

static inline uint8_t pickAngleForTarget(float gPortion, float maxSec){
  float bestErr=1e9; uint8_t best=90;
  struct X{uint8_t ang; float r;}; X A[3]={{90,RATE_90},{100,RATE_100},{180,RATE_180}};
  for(auto &x:A){
    float t=gPortion/x.r; if(t<0.3f)t=0.3f; if(t>maxSec)t=maxSec;
    float err=fabsf(x.r*t-gPortion);
    if(err<bestErr){bestErr=err;best=x.ang;}
  }
  return best;
}
static inline uint16_t secondsToDeci(float s){
  int d=(int)roundf(s*10.0f); if(d<1)d=1; if(d>200)d=200; return (uint16_t)d;
}

/* ====== CALCULO PRINCIPAL ====== */
static inline CalcOut computePlan(){
  CalcOut c{0,0,1,90,10};
  float kg = settings.weight10/10.0f;

  if(settings.foodType==FT_CRIADORES){
    if(settings.dogType==DT_PUP){
      const uint16_t* row = CRI_P_3_6;
      if(settings.pupAge==PUP_2M)       row = CRI_P_2M;
      else if(settings.pupAge==PUP_6_12) row = CRI_P_6_12;
      float kgClamp = kg; if(kgClamp<CRI_W[0]) kgClamp=CRI_W[0]; if(kgClamp>CRI_W[5]) kgClamp=CRI_W[5];
      c.gDay = interpCri(CRI_W, row, 6, kgClamp);
      c.portions = 4;
    }else{
      const uint16_t* row = (settings.activity==ACT_INT)?CRI_AD_INT:CRI_AD_MOD;
      if(kg <= 50.0f){
        float kgClamp = kg; if(kgClamp<CRI_W[0]) kgClamp=CRI_W[0];
        c.gDay = interpCri(CRI_W, row, 6, kgClamp);
      }else{
        c.gDay = criAdultOver50Extra(kg, (Activity)settings.activity);
      }
      c.portions = (settings.dogType==DT_ADULT_BIG)?1:2;
    }
  }else{ // FT_PUP_SMALL (mordida pequeña) — sin 12+ meses
    const PupRow& r=rowForKg(kg);
    uint16_t g=0;
    switch(settings.pupAge){
      case PUP_DST_3: g = r.g2m;   break; // Destete–3
      case PUP_3_6:   g = r.g3_5;  break; // 3–5
      case PUP_5_8:   g = r.g5_8;  break; // 5–8
      case PUP_8_12:  g = r.g8_12; break; // 8–12
      default:        g = r.g8_12; break; // fallback seguro
    }
    c.portions=4; c.gDay=g;
  }

  if(c.portions<1)c.portions=1;
  c.gPortion=(uint16_t)max(1,(int)roundf((float)c.gDay/(float)c.portions));

  uint8_t ang=pickAngleForTarget((float)c.gPortion,3.0f);
  float rate=(ang==90)?RATE_90:((ang==100)?RATE_100:RATE_180);
  c.angle=ang; c.deci=secondsToDeci(((float)c.gPortion)/rate);
  return c;
}

/* ================= SERVO ================== */
void moveServoTo(int p){ servo.write(p); delay(20); }
void dispenseDeci(uint8_t angle,uint16_t deci){
  moveServoTo(servoClosed); delay(50);
  moveServoTo(angle);       delay(deci*100);
  moveServoTo(servoClosed);
}

/* ============== UI HTML/CSS/JS =========== */
String htmlPage(){
  String s = R"HTML(<!DOCTYPE html><html><head>
<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Comedero</title>
<style>
*{box-sizing:border-box}
body{background:#0b1220;color:#e5e7eb;font-family:system-ui,Arial,Helvetica,sans-serif;margin:0}
.wrap{max-width:720px;margin:18px auto;padding:12px}
.badge{display:inline-flex;align-items:center;gap:8px;background:#0f2238;border:1px solid #1f2f4a;padding:8px 12px;border-radius:12px;margin:6px 6px 0 0;white-space:nowrap}
.card{background:#0f192b;border:1px solid #192640;border-radius:14px;padding:14px;margin:14px 0}
h2{margin:6px 0 10px 0}
.row{display:flex;gap:12px;flex-wrap:wrap}
.row>div{flex:1 1 240px;min-width:180px}
label{display:block;font-size:13px;color:#9fb1ca;margin:4px 0 6px}
input,select,button{width:100%;padding:12px;border-radius:10px;border:1px solid #23344f;background:#0c1424;color:#e5e7eb;font-size:16px}
input,select{height:48px}
input[type=number]{text-align:center}
button.primary{background:#3b82f6;border-color:#4b9bff;color:white}
.small{color:#9fb1ca;font-size:13px}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:520px){.grid2{grid-template-columns:1fr}}
.gridH{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:12px}
.slot{background:#0d1628;border:1px solid #1e2c45;border-radius:12px;padding:10px}
.hrTitle{font-size:13px;color:#9fb1ca;margin:2px 0 8px}
.hhmm{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.hhmm input{width:100%}
.headerRow{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.switch{display:inline-flex;align-items:center;gap:8px}
.switch input{width:auto;height:auto}
</style></head><body><div class='wrap'>

<div class='card'>
  <div class='headerRow'>
    <h2 style="margin:0">Comedero Automatico</h2>
    <div class='badge' id='now'>--:--:--</div>
    <div class='badge'>UTC-3</div>
    <div class='badge'>UI v2.4.1</div>
  </div>
  <div class='row' style="margin-top:8px">
    <div><button class='primary' onclick='refresh(true)'>Refrescar estado</button></div>
    <div class='switch'>
      <label><input type='checkbox' id='autoChk'> Auto-refresh (30 s)</label>
    </div>
  </div>
</div>

<div class='card'>
  <div class='row'>
    <div class='badge'>Alimento: <b id='sumFood'>-</b></div>
    <div class='badge'>Tipo: <b id='sumType'>-</b></div>
    <div class='badge'>Peso: <b id='sumW'>-</b> kg</div>
    <div class='badge'>Edad: <b id='sumAge'>-</b></div>
    <div class='badge'>Porciones/dia: <b id='sumN'>-</b></div>
  </div>
  <div class='row'>
    <div class='badge'>Recomendacion: <b id='sumGd'>-</b> g/dia</div>
    <div class='badge'>Por porcion: <b id='sumGp'>-</b> g</div>
    <div class='badge'>Sugerido: <b id='sumSug'>-</b></div>
  </div>
  <div class='small' id='sumRac'>Raciones: -</div>
</div>

<form class='card' id='f' onsubmit='save();return false;'>

<h3 style="margin-top:0">0) Tipo de alimento</h3>
<div class='row'>
  <div>
    <label>Alimento</label>
    <select id='ft'>
      <option value='0'>Sieger Criadores</option>
      <option value='1'>Sieger Cachorros (mordida pequeña)</option>
    </select>
  </div>
</div>

<h3>1) Perfil</h3>
<div class='row' id='typeRow'>
  <div>
    <label>Tipo de perro (solo Criadores)</label>
    <select id='dt'>
      <option value='0'>Cachorro</option>
      <option value='1'>Adulto med/grand</option>
      <option value='2'>Adulto chico</option>
    </select>
  </div>
  <div id='actCol'>
    <label>Actividad (adultos)</label>
    <select id='act'>
      <option value='0'>Moderada</option>
      <option value='1'>Intensiva</option>
    </select>
  </div>
</div>

<div class='row'>
  <div>
    <label>Peso (kg)</label>
    <input type='number' step='0.1' min='0' max='200' id='w'/>
  </div>
  <div id='ageCol'>
    <label>Edad (meses)</label>
    <select id='pa'></select>
  </div>
</div>

<h3>2) Plan de racion</h3>
<div class='grid2'>
  <div><label>g/dia</label><input id='gd' readonly></div>
  <div><label>g/porcion</label><input id='gp' readonly></div>
</div>

<h3>3) Programacion</h3>
<div class='gridH'>
  <div class='slot'>
    <div class='hrTitle'>R1</div>
    <div class='hhmm'>
      <input type='number' min='0' max='23' id='f1h' placeholder='HH'>
      <input type='number' min='0' max='59' id='f1m' placeholder='MM'>
    </div>
  </div>
  <div class='slot r2'>
    <div class='hrTitle'>R2</div>
    <div class='hhmm'>
      <input type='number' min='0' max='23' id='f2h' placeholder='HH'>
      <input type='number' min='0' max='59' id='f2m' placeholder='MM'>
    </div>
  </div>
  <div class='slot r3'>
    <div class='hrTitle'>R3</div>
    <div class='hhmm'>
      <input type='number' min='0' max='23' id='f3h' placeholder='HH'>
      <input type='number' min='0' max='59' id='f3m' placeholder='MM'>
    </div>
  </div>
  <div class='slot r4'>
    <div class='hrTitle'>R4</div>
    <div class='hhmm'>
      <input type='number' min='0' max='23' id='f4h' placeholder='HH'>
      <input type='number' min='0' max='59' id='f4m' placeholder='MM'>
    </div>
  </div>
</div>

<div class='row' style="margin-top:8px">
  <div><button type='submit' class='primary'>Guardar</button></div>
  <div><button type='button' onclick='feed()'>Dar ahora</button></div>
</div>
</form>

<script>
function fmt2(n){return (n<10?'0':'')+n;}
function setText(id,t){document.getElementById(id).textContent=t;}
function setVal(id,v){const el=document.getElementById(id); if(el) el.value=v;}

let autoTimer=null, AUTO_MS=30000, editing=false, editTO=null;

function startAuto(){
  if(autoTimer) clearInterval(autoTimer);
  if(document.getElementById('autoChk').checked){
    autoTimer=setInterval(()=>{ if(!editing) refresh(false); }, AUTO_MS);
  }
}

async function refresh(force){
  try{
    const r=await fetch('/status.json'+(force?'?t='+Date.now():'')); const S=await r.json();
    setText('now',S.now||'--:--:--');
    setVal('ft',S.ft); setVal('dt',S.dt); setVal('act',S.act);
    setVal('w',(S.w/10).toFixed(1));
    setVal('f1h',S.f1h); setVal('f1m',S.f1m);
    setVal('f2h',S.f2h); setVal('f2m',S.f2m);
    setVal('f3h',S.f3h); setVal('f3m',S.f3m);
    setVal('f4h',S.f4h); setVal('f4m',S.f4m);
    buildAgeOptions(+S.ft); setVal('pa',S.pa);
    applyMode(); await calcLive();
  }catch(e){console.log(e);}
}

function buildAgeOptions(ft){
  const pa=document.getElementById('pa'); pa.innerHTML='';
  function add(v,t){const o=document.createElement('option');o.value=v;o.text=t;pa.add(o);}
  if(ft===0){ add(0,'2 meses'); add(1,'3-6 meses'); add(2,'6-12 meses'); }
  else{ add(3,'Destete-3 meses'); add(1,'3-5 meses'); add(4,'5-8 meses'); add(5,'8-12 meses'); }
  /* ya no existe “12+ meses” */
}

function portionsCount(){
  const ft=+document.getElementById('ft').value;
  if(ft===1) return 4;
  const dt=+document.getElementById('dt').value;
  if(dt===0) return 4;
  if(dt===1) return 1;
  return 2;
}

function applyMode(){
  const ft=+document.getElementById('ft').value;
  const dtRow=document.getElementById('typeRow');
  const actCol=document.getElementById('actCol');
  const ageCol=document.getElementById('ageCol');
  if(ft===0){
    dtRow.style.display='flex';
    const dt=+document.getElementById('dt').value;
    actCol.style.display=(dt===0)?'none':'block';
    ageCol.style.display=(dt===0)?'block':'none';
  }else{
    dtRow.style.display='none'; actCol.style.display='none'; ageCol.style.display='block';
  }
  const n=portionsCount();
  document.querySelectorAll('.r2,.r3,.r4').forEach(el=>el.style.display='none');
  if(n>=2) document.querySelectorAll('.r2').forEach(el=>el.style.display='block');
  if(n>=3) document.querySelectorAll('.r3').forEach(el=>el.style.display='block');
  if(n>=4) document.querySelectorAll('.r4').forEach(el=>el.style.display='block');
}

async function calcLive(){
  const q=[
    'ft='+document.getElementById('ft').value,
    'dt='+document.getElementById('dt').value,
    'act='+document.getElementById('act').value,
    'pa='+document.getElementById('pa').value,
    'w='+document.getElementById('w').value
  ].join('&');
  const r=await fetch('/calc?'+q); const j=await r.json();
  setVal('gd',j.gd); setVal('gp',j.gp);
  setText('sumFood',(+document.getElementById('ft').value===0)?'Criadores':'Cachorros peq.');
  const ft=+document.getElementById('ft').value;
  let typeTxt='-';
  if(ft===0){ const dt=+document.getElementById('dt').value;
    typeTxt=(dt===0)?'Cachorro':((dt===1)?'Adulto med/grand':'Adulto chico'); }
  else typeTxt='Cachorro';
  setText('sumType',typeTxt);
  setText('sumW',document.getElementById('w').value);
  const pa=document.getElementById('pa'); setText('sumAge',pa.options.length?pa.options[pa.selectedIndex].text:'-');
  setText('sumN',portionsCount());
  setText('sumGd',j.gd); setText('sumGp',j.gp);
  setText('sumSug',j.ang+' deg, '+(j.deci/10).toFixed(1)+' s');
  const times=[[f1h.value,f1m.value],[f2h.value,f2m.value],[f3h.value,f3m.value],[f4h.value,f4m.value]];
  let n=portionsCount(), txt=[];
  for(let i=0;i<n;i++) txt.push('R'+(i+1)+': '+fmt2(+times[i][0])+':'+fmt2(+times[i][1])+' - '+j.gp+' g');
  setText('sumRac',txt.join(' / '));
}

function bindLive(){
  const form=document.getElementById('f');
  function touch(){ editing=true; if(editTO) clearTimeout(editTO); editTO=setTimeout(()=>editing=false,20000); }
  form.addEventListener('input',touch);
  form.addEventListener('focusin',touch);
  ['ft','dt','act','pa','w','f1h','f1m','f2h','f2m','f3h','f3m','f4h','f4m'].forEach(id=>{
    const el=document.getElementById(id); if(!el)return;
    el.addEventListener('input',()=>{ if(id==='ft'){buildAgeOptions(+ft.value);} applyMode(); calcLive(); });
  });
  document.getElementById('autoChk').addEventListener('change',startAuto);
}

async function save(){
  const body=new URLSearchParams();
  ['ft','dt','act','pa','w','f1h','f1m','f2h','f2m','f3h','f3m','f4h','f4m'].forEach(id=>body.append(id,document.getElementById(id).value));
  await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});
  await refresh(true);
}
async function feed(){
  const q=['ft='+ft.value,'dt='+dt.value,'act='+act.value,'pa='+pa.value,'w='+w.value].join('&');
  const r=await fetch('/calc?'+q); const j=await r.json();
  await fetch('/feed?ang='+j.ang+'&deci='+j.deci);
}
refresh(true); bindLive(); startAuto();
</script>

</div></body></html>)HTML";
  return s;
}

/* ============== HTTP ============== */
void handleRoot(){ server.send(200,"text/html",htmlPage()); }
void handleStatus(){
  time_t t=time(nullptr); struct tm* ti=localtime(&t);
  char nowb[12]; if(ti) sprintf(nowb,"%02d:%02d:%02d",ti->tm_hour,ti->tm_min,ti->tm_sec); else strcpy(nowb,"--:--:--");
  String j="{\"now\":\""+String(nowb)+"\",\"ft\":"+settings.foodType+",\"dt\":"+settings.dogType+
           ",\"act\":"+settings.activity+",\"pa\":"+settings.pupAge+",\"w\":"+settings.weight10+
           ",\"f1h\":"+settings.f1h+",\"f1m\":"+settings.f1m+",\"f2h\":"+settings.f2h+",\"f2m\":"+settings.f2m+
           ",\"f3h\":"+settings.f3h+",\"f3m\":"+settings.f3m+",\"f4h\":"+settings.f4h+",\"f4m\":"+settings.f4m+"}";
  server.send(200,"application/json",j);
}
void handleCalc(){
  Settings tmp=settings;
  if(server.hasArg("ft")) tmp.foodType=(uint8_t)server.arg("ft").toInt();
  if(server.hasArg("dt")) tmp.dogType=(uint8_t)server.arg("dt").toInt();
  if(server.hasArg("act")) tmp.activity=(uint8_t)server.arg("act").toInt();
  if(server.hasArg("pa")) tmp.pupAge=(uint8_t)server.arg("pa").toInt();
  if(server.hasArg("w"))  tmp.weight10=(uint16_t)roundf(server.arg("w").toFloat()*10.0f);
  Settings bak=settings; settings=tmp; CalcOut c=computePlan(); settings=bak;
  String j="{\"gd\":"+String(c.gDay)+",\"gp\":"+String(c.gPortion)+",\"ang\":"+String(c.angle)+",\"deci\":"+String(c.deci)+"}";
  server.send(200,"application/json",j);
}
void handleSave(){
  if(server.hasArg("ft")) settings.foodType=(uint8_t)server.arg("ft").toInt();
  if(server.hasArg("dt")) settings.dogType=(uint8_t)server.arg("dt").toInt();
  if(server.hasArg("act")) settings.activity=(uint8_t)server.arg("act").toInt();
  if(server.hasArg("pa")) settings.pupAge=(uint8_t)server.arg("pa").toInt();
  if(server.hasArg("w"))  settings.weight10=(uint16_t)roundf(server.arg("w").toFloat()*10.0f);
  if(server.hasArg("f1h")) settings.f1h=(uint8_t)server.arg("f1h").toInt();
  if(server.hasArg("f1m")) settings.f1m=(uint8_t)server.arg("f1m").toInt();
  if(server.hasArg("f2h")) settings.f2h=(uint8_t)server.arg("f2h").toInt();
  if(server.hasArg("f2m")) settings.f2m=(uint8_t)server.arg("f2m").toInt();
  if(server.hasArg("f3h")) settings.f3h=(uint8_t)server.arg("f3h").toInt();
  if(server.hasArg("f3m")) settings.f3m=(uint8_t)server.arg("f3m").toInt();
  if(server.hasArg("f4h")) settings.f4h=(uint8_t)server.arg("f4h").toInt();
  if(server.hasArg("f4m")) settings.f4m=(uint8_t)server.arg("f4m").toInt();
  CalcOut c=computePlan(); settings.suggAngle=c.angle; settings.suggDeci=c.deci;
  server.send(200,"text/plain","OK");
}
void handleFeed(){
  // Mantengo compatibilidad: si llegan ang/deci por query, los uso;
  // igual calculo el plan para saber g/porción y g/día (para ThingSpeak)
  CalcOut c = computePlan();

  uint8_t ang = settings.suggAngle;
  uint16_t deci = settings.suggDeci;
  if(server.hasArg("ang"))  ang  = (uint8_t)server.arg("ang").toInt();
  if(server.hasArg("deci")) deci = (uint16_t)server.arg("deci").toInt();

  dispenseDeci(ang, deci);

  // Como es "Dar ahora" manual, no sabemos qué ración es; mando 0.
  tsUpdate((int)c.gPortion, /*portion_no*/0, (int)c.gPortion, (int)c.gDay);

  server.send(200,"text/plain","FED");
}

/* ============ SCHEDULER ============ */
void maybeRunSchedules(){
  if(!timeReady) return;
  time_t t=time(nullptr); struct tm* ti=localtime(&t); if(!ti) return;
  static int lastMin=-1; static bool fired[4]={false,false,false,false};
  if(lastMin!=ti->tm_min){for(int i=0;i<4;i++)fired[i]=false; lastMin=ti->tm_min;}
  uint8_t N=1;
  if(settings.foodType==FT_CRIADORES){
    if(settings.dogType==DT_PUP) N=4;
    else N=(settings.dogType==DT_ADULT_BIG)?1:2;
  }else N=4;
  struct H{uint8_t h,m;} Hs[4]={
    {settings.f1h,settings.f1m},{settings.f2h,settings.f2m},
    {settings.f3h,settings.f3m},{settings.f4h,settings.f4m}
  };
  for(uint8_t i=0;i<N;i++){
    if(!fired[i] && ti->tm_hour==Hs[i].h && ti->tm_min==Hs[i].m && ti->tm_sec==0){
      CalcOut c=computePlan(); dispenseDeci(c.angle,c.deci); fired[i]=true;
      tsUpdate((int)c.gPortion, (int)(i+1), (int)c.gPortion, (int)c.gDay);
    }
  }
}

/* ============== SETUP/LOOP ============== */
void setup(){
  Serial.begin(115200); delay(100);
  servo.attach(SERVO_PIN); servo.write(servoClosed);
  WiFi.mode(WIFI_STA); WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED){ delay(500); }
  configTime(gmtOffset_sec,0,"pool.ntp.org","time.nist.gov");
  for(int i=0;i<20;i++){ if(time(nullptr)>100000){timeReady=true;break;} delay(500); }
  server.on("/",handleRoot);
  server.on("/status.json",handleStatus);
  server.on("/calc",handleCalc);
  server.on("/save",HTTP_POST,handleSave);
  server.on("/feed",handleFeed);
  server.begin();
}
void loop(){
  server.handleClient();
  maybeRunSchedules();
}
