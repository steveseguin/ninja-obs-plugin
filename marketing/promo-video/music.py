import numpy as np, wave
SR=44100; DUR=88.0; N=int(SR*DUR); BPM=120; B=60/BPM
rng=np.random.default_rng(7)
L=np.zeros(N); R=np.zeros(N)
def f(m): return 440*2**((m-69)/12)
def add(sig,t,pan=0.0,g=1.0):
    i=int(t*SR); j=min(N,i+len(sig))
    if i>=N or j<=i: return
    s=sig[:j-i]*g
    L[i:j]+=s*np.sqrt((1-pan)/2); R[i:j]+=s*np.sqrt((1+pan)/2)
def env(n,a,d,s=0.0,rel=None):
    t=np.arange(n)/SR
    e=np.where(t<a,t/a,np.exp(-(t-a)/d)*(1-s)+s)
    if rel: e*=np.clip((n/SR-t)/rel,0,1)
    return e
def osc(freq,dur,kind='saw',detune=0):
    t=np.arange(int(dur*SR))/SR; out=0
    for dt in ([0] if not detune else [-detune,0,detune]):
        ph=(f_:=freq*(1+dt))*t
        out=out+(2*(ph%1)-1 if kind=='saw' else np.sin(2*np.pi*ph) if kind=='sin' else np.sign(np.sin(2*np.pi*ph)))
    return out/ (1 if not detune else 3)
def lp(x,cut):
    a=np.exp(-2*np.pi*cut/SR); y=np.zeros_like(x); acc=0
    # vectorised one-pole via lfilter substitute
    from itertools import accumulate
    return np.array(list(accumulate(x*(1-a),lambda p,v:p*a+v)))
def kick():
    n=int(.35*SR); t=np.arange(n)/SR
    fr=50+120*np.exp(-t*30); ph=2*np.pi*np.cumsum(fr)/SR
    return np.sin(ph)*np.exp(-t*9)*1.0
def clap():
    n=int(.25*SR); t=np.arange(n)/SR
    return rng.standard_normal(n)*np.exp(-t*22)*.45
def hat(o=False):
    n=int((.2 if o else .05)*SR); t=np.arange(n)/SR
    x=rng.standard_normal(n); x=np.diff(x,prepend=0)
    return x*np.exp(-t*(15 if o else 80))*.18
KICK,CLAP,HAT,OHAT=kick(),clap(),hat(),hat(True)
# chords (A minor): Am F C G
CH=[[57,60,64],[53,57,60],[48,52,55],[55,59,62]]
ROOT=[45,41,48,43]
def section(t):
    if t<6: return 'intro'
    if 56<=t<64: return 'break'
    if t>=86: return 'out'
    return 'full'
bars=int(DUR/(4*B))
pad_cache={}
for bar in range(bars):
    t0=bar*4*B; ch=CH[bar%4]; rt=ROOT[bar%4]; sec=section(t0)
    # pad
    if sec!='out' or t0<87:
        dur=4*B+.6
        p=sum(osc(f(m),dur,'saw',.004) for m in ch+[ch[0]+12])
        p=lp(p,900 if sec=='full' else 600)*env(len(p),.35,9,.8,.5)*(.05 if sec=='full' else .13)
        add(p,t0,-.2 if bar%2 else .2)
    # arp
    pat=[0,1,2,3,2,1,0,2]
    notes=ch+[ch[0]+12]
    for k in range(16):
        tt=t0+k*B/4
        if sec=='intro' and tt<2: continue
        m=notes[pat[k%8]]+12+(12 if (k//8)%2 and sec!='intro' else 0)
        s=osc(f(m),.22,'sq')*env(int(.22*SR),.002,.07)
        s=lp(s,2500 if sec!='intro' else 700+tt*300)
        g=.045 if sec=='full' else (.08 if sec=='break' else .07*min(1,tt/5))
        add(s,tt,.5*np.sin(k*.8),g)
        add(s,tt+.375,-.5*np.sin(k*.8),g*.35)  # dotted-8th echo
    if sec in('full',):
        for beat in range(4):
            tb=t0+beat*B
            add(KICK,tb,0,.9)
            if beat in(1,3): add(CLAP,tb,.1,1)
            add(HAT,tb+B/2,.3,1)
            add(HAT,tb+B/4,-.3,.5); add(HAT,tb+3*B/4,-.3,.5)
            if beat==3 and bar%4==3: add(OHAT,tb+B/2,.2,1)
            # bass offbeat
            for q in (B/2,3*B/4):
                b=osc(f(rt-12),.2,'saw')+.5*osc(f(rt-24),.2,'sin')
                b=lp(b,500)*env(len(b),.004,.12)*.22
                add(b,tb+q,0)
            b=lp(osc(f(rt-24),.25,'sin'),300)*env(int(.25*SR),.003,.2)*.25
            add(b,tb,0)
# risers into 6, 64, 80
def riser(t_end,d):
    n=int(d*SR); t=np.arange(n)/SR
    x=rng.standard_normal(n)
    cut=200+7000*(t/d)**2
    y=np.zeros(n); a=0
    # time-varying one-pole
    alpha=np.exp(-2*np.pi*cut/SR); acc=0
    for i in range(0,n,64):
        seg=x[i:i+64]; al=alpha[i]
        for v in seg: acc=acc*al+v*(1-al)
        y[i:i+64]=acc  # coarse but fine
    y=lp(x,1)*0  # placeholder removed below
    return None
def riser2(t_end,d,g=.25):
    n=int(d*SR); t=np.arange(n)/SR
    x=rng.standard_normal(n)
    hi=np.diff(x,prepend=0)
    mix=(t/d)
    y=(x*(1-mix)*.3+hi*mix)*(t/d)**2*g
    sw=np.sin(2*np.pi*np.cumsum(200+1800*(t/d)**2)/SR)*(t/d)**2*g*.3
    add(y+sw,t_end-d,0)
riser2(6,4); riser2(64,4); riser2(80,3.5)
def impact(t):
    n=int(2.5*SR); tt=np.arange(n)/SR
    boom=np.sin(2*np.pi*np.cumsum(40+80*np.exp(-tt*8))/SR)*np.exp(-tt*2.2)*.8
    nz=rng.standard_normal(n)*np.exp(-tt*6)*.12
    add(boom+nz,t,0)
impact(6); impact(64); impact(80)
# scene whooshes
for t in (12,20,32,40,48,56,74):
    n=int(.6*SR); tt=np.arange(n)/SR
    w=rng.standard_normal(n)*np.sin(np.pi*tt/.6)**2*.08
    add(w,t-.35,.4); add(w[::-1]*.6,t-.35,-.4)
# final chord ring
fin=sum(osc(f(m),5,'saw',.005) for m in [57,60,64,69,76])
fin=lp(fin,1200)*env(len(fin),.02,1.6,0,1.0)*.07
add(fin,86,0)
add(KICK,86,0,.9); impact(86)
# simple reverb
def verb(x):
    y=x.copy()
    for d,g in ((.031,.35),(.047,.3),(.071,.25),(.113,.2),(.163,.15),(.241,.1)):
        k=int(d*SR); y[k:]+=x[:-k]*g
    return y
L2=verb(L)*.8+L*.2; R2=verb(R[::1])*.8+R*.2
st=np.stack([L2,R2],1)
# master fade and limiter
t=np.arange(N)/SR
st*= np.clip((DUR-t)/1.2,0,1)[:,None]
st/=np.max(np.abs(st))*1.02
st=np.tanh(st*1.6)/np.tanh(1.6)
w=wave.open('music.wav','wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
w.writeframes((st*32000).astype('<i2').tobytes()); w.close()
print('done')
