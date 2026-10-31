"""Render the original vector mixer icon at multiple Windows icon sizes."""
from pathlib import Path
from PIL import Image, ImageDraw
root = Path(__file__).resolve().parent
assets = root / 'Assets'
assets.mkdir(exist_ok=True)
(assets/'jsrf-mixer.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><rect x="8" y="8" width="240" height="240" rx="54" fill="#1d2024"/><path d="M52 137v-18a76 76 0 0 1 152 0v18" fill="none" stroke="#3fdac4" stroke-width="17" stroke-linecap="round"/><rect x="39" y="119" width="30" height="62" rx="13" fill="#3fdac4"/><rect x="187" y="119" width="30" height="62" rx="13" fill="#3fdac4"/><rect x="89" y="142" width="18" height="59" rx="9" fill="#3fdac4"/><rect x="119" y="110" width="18" height="91" rx="9" fill="#ff993c"/><rect x="149" y="156" width="18" height="45" rx="9" fill="#3fdac4"/></svg>''')
im=Image.new('RGBA',(1024,1024),(0,0,0,0));d=ImageDraw.Draw(im)
def box(v):return tuple(int(x*4) for x in v)
def rr(v,r,c):d.rounded_rectangle(box(v),radius=r*4,fill=c)
rr((8,8,248,248),54,'#1d2024')
d.arc(box((52,43,204,195)),180,360,fill='#3fdac4',width=68)
d.line(box((52,119,52,137)),fill='#3fdac4',width=68)
d.line(box((204,119,204,137)),fill='#3fdac4',width=68)
rr((39,119,69,181),13,'#3fdac4');rr((187,119,217,181),13,'#3fdac4')
rr((89,142,107,201),9,'#3fdac4');rr((119,110,137,201),9,'#ff993c');rr((149,156,167,201),9,'#3fdac4')
im=im.resize((256,256),Image.Resampling.LANCZOS)
im.save(assets/'jsrf-mixer.png')
im.save(assets/'jsrf-mixer.ico',sizes=[(n,n) for n in (16,20,24,32,48,64,128,256)])
print('Original vector icon rendered: 16, 20, 24, 32, 48, 64, 128, 256 px')
