# Makes the pictures for images.html (run once; the results are committed)
from PIL import Image, ImageDraw
def photo(w, h):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (int(40 + 180 * x / w), int(90 + 120 * y / h), int(200 - 120 * x / w))
    d = ImageDraw.Draw(im)
    d.ellipse((w * 0.6, h * 0.1, w * 0.85, h * 0.5), fill=(250, 220, 90))
    d.polygon([(0, h), (w * 0.35, h * 0.45), (w * 0.7, h)], fill=(40, 70, 50))
    d.polygon([(w * 0.4, h), (w * 0.75, h * 0.55), (w, h * 0.9), (w, h)], fill=(30, 55, 40))
    return im
photo(1600, 1000).save("photo.jpg", quality=85)
photo(400, 250).save("progressive.jpg", quality=80, progressive=True)
logo = Image.new("RGBA", (220, 80), (0, 0, 0, 0))
d = ImageDraw.Draw(logo)
d.ellipse((4, 4, 76, 76), fill=(43, 108, 176, 255))
d.ellipse((24, 24, 56, 56), fill=(242, 201, 76, 200))
d.rectangle((90, 22, 210, 58), fill=(229, 62, 62, 160))
logo.save("logo.png")
g = Image.new("P", (120, 60))
g.putpalette([255, 255, 255, 220, 40, 40, 40, 160, 60, 40, 80, 200] + [0] * 756)
dg = ImageDraw.Draw(g)
for i in range(3):
    dg.rectangle((8 + i * 38, 10, 36 + i * 38, 50), fill=i + 1)
g.save("shapes.gif")
b = Image.new("RGB", (64, 64), (255, 255, 255))
db = ImageDraw.Draw(b)
for i in range(4):
    db.rectangle((i * 8, i * 8, 63 - i * 8, 63 - i * 8), outline=(i * 60, 100, 255 - i * 60), width=3)
b.save("icon.bmp")
grad = Image.new("L", (256, 40))
grad.putdata([x for y in range(40) for x in range(256)])
grad.save("gray.png")
tri = Image.new("RGBA", (10, 10), (0, 0, 0, 0))
ImageDraw.Draw(tri).polygon([(5, 1), (9, 9), (1, 9)], fill=(153, 153, 153, 255))
tri.save("triangle.png")
