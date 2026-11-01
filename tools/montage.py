import sys, os
from PIL import Image, ImageDraw
d=sys.argv[1]; out=sys.argv[2]; files=sorted(f for f in os.listdir(d) if f.endswith('.png'))
cols=5; W,H=240,172; rows=(len(files)+cols-1)//cols
m=Image.new('RGB',(cols*W,rows*H),(40,40,40)); dr=ImageDraw.Draw(m)
for i,f in enumerate(files):
    x,y=(i%cols)*W,(i//cols)*H; m.paste(Image.open(os.path.join(d,f)),(x,y+12)); dr.text((x+2,y),f[:-4][:38],fill=(255,255,0))
m.save(out)
