"""Host preview of the packaged Android controller JSON (requires Pillow).
python -B tools/render_controller.py [--popup] [--output work/gamepad/overlay.png]
Coordinates, radius, opacity, labels and text fitting mirror Ac1ControllerView.
The neutral backdrop is a preview aid, not a claim of an Android screenshot.
"""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--layout", type=Path, default=ROOT / "android/app/src/main/assets/controller-layout.json")
p.add_argument("--output", type=Path, default=ROOT / "work/gamepad/overlay.png")
p.add_argument("--popup", action="store_true")
p.add_argument("--width", type=int, default=3120)
p.add_argument("--height", type=int, default=1440)
args = p.parse_args()
j = json.loads(args.layout.read_text())
w, h = args.width, args.height
margin = 24  # 8 dp at a representative 3x density; Android also subtracts insets.
sw, sh = w-2*margin, h-2*margin
unit = min(sw, sh)
image = Image.new("RGB", (w,h))
draw = ImageDraw.Draw(image)
for y in range(h):
    t=y/h
    draw.line((0,y,w,y), fill=(int(28+12*t),int(39+14*t),int(47+17*t)))
layer = Image.new("RGBA", (w,h))
draw = ImageDraw.Draw(layer)
font_path = "C:/Windows/Fonts/arial.ttf"
def text(label, x, baseline, size, max_width=None, color=(210,225,235)):
    font=ImageFont.truetype(font_path,max(8,round(size)))
    if max_width and draw.textlength(label,font=font)>max_width:
        font=ImageFont.truetype(font_path,max(8,int(size*max_width/draw.textlength(label,font=font))))
    draw.text((round(x),round(baseline)),label,font=font,anchor="ms",fill=(*color,round(j["opacity"]*255)))
def control(c):
    x,y=margin+c["x"]*sw, margin+c["y"]*sh
    r=c["r"]*unit
    draw.ellipse((round(x-r),round(y-r),round(x+r),round(y+r)),
        fill=(20,30,39,round(j["opacity"]*190)), outline=(210,225,235,round(j["opacity"]*255)),width=max(1,round(unit*.0015)))
    if c["id"]=="move":
        r*=.33;draw.ellipse((round(x-r),round(y-r),round(x+r),round(y+r)),fill=(210,225,235,round(j["opacity"]*255)))
    else:
        size=unit*(.019 if c["r"]<.04 else .025)
        text(c["label"],x,y+size*.34,size,max_width=r*1.75)
        if c["id"] in ("A","B","X","Y"):text(c["id"],x,y-r*.42,unit*.025)
for c in j["controls"]:
    if not (args.popup and c["id"]=="weapons"):control(c)
if args.popup:
    for c in j["weapons"]:control(c)
text("Swipe to look",margin+sw*.59,margin+sh*.59,unit*.022,color=(225,235,242))
image=Image.alpha_composite(image.convert("RGBA"),layer).convert("RGB")
args.output.parent.mkdir(parents=True,exist_ok=True)
image.save(args.output)
print(f"{args.output} ({w}x{h}; packaged JSON; no device execution)")
