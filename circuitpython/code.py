# Flight tracker: hardware smoke test.
# Draws a border and a label on the 64x32 HUB75 panel. No network yet.
import board
import displayio
import framebufferio
import rgbmatrix
import terminalio
from adafruit_display_text import label

WIDTH, HEIGHT = 64, 32

displayio.release_displays()

matrix = rgbmatrix.RGBMatrix(
    width=WIDTH,
    height=HEIGHT,
    bit_depth=4,
    rgb_pins=[
        board.MTX_R1, board.MTX_G1, board.MTX_B1,
        board.MTX_R2, board.MTX_G2, board.MTX_B2,
    ],
    addr_pins=[board.MTX_ADDRA, board.MTX_ADDRB, board.MTX_ADDRC, board.MTX_ADDRD],
    clock_pin=board.MTX_CLK,
    latch_pin=board.MTX_LAT,
    output_enable_pin=board.MTX_OE,
)
display = framebufferio.FramebufferDisplay(matrix, auto_refresh=True)

# Border: bitmap with a 1-px frame in colour index 1.
bitmap = displayio.Bitmap(WIDTH, HEIGHT, 2)
palette = displayio.Palette(2)
palette[0] = 0x000000
palette[1] = 0x0040FF
for x in range(WIDTH):
    bitmap[x, 0] = 1
    bitmap[x, HEIGHT - 1] = 1
for y in range(HEIGHT):
    bitmap[0, y] = 1
    bitmap[WIDTH - 1, y] = 1

group = displayio.Group()
group.append(displayio.TileGrid(bitmap, pixel_shader=palette))
group.append(label.Label(terminalio.FONT, text="FLIGHT", color=0xFFFFFF, x=12, y=10))
group.append(label.Label(terminalio.FONT, text="TRACKER", color=0xFFB000, x=8, y=22))
display.root_group = group

print("panel ok")
while True:
    pass
