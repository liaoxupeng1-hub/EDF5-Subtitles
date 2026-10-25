# EDF6 subtitle layout evidence

Target: `EDF.dll (EDF6)`

SHA-256: `0D5092393A1B9D8F1B75B0C93D8A8F65615A36A92C46E40349C2B66C733D0916`.

Static PE inspection and objdump only; no game execution. Addresses below are preferred virtual addresses (image base `0x180000000`). These observations are not REA Evidence IDs. The original DLL was not modified.

## Directly established layout

- Renderer: `0x1807D8940` through `0x1807D92AC`.
- Scale factors: `sx = viewportWidth / 1920`, `sy = viewportHeight / 1080` (`7D8A01`–`7D8A2D`; constants at `1817F42D8=1920`, `181792258=1080`).
- Text measurement/wrap rectangle copied from globals `181F776C0/C4/C8/CC`: `(0, 0, 769.2307739257812, 10000)` in reference coordinates. Renderer multiplies its width and height by `sx/sy` respectively (`7D8A98`–`7D8AD1`).
- Background content width: `max(viewportWidth * 0.25, max(activeNode.width) * sx)` (`7D8C0E`–`7D8C40`). Left edge is `(viewportWidth - width)/2` (`7D8C45`–`7D8C5F`).
- Background content origin Y: `viewportHeight - viewportHeight / 3`, i.e. `2*H/3` (`7D8C63`–`7D8C6E`; `181C369BC=3.0`). At 1080 height this is 720. An earlier informal message incorrectly read the divisor as 8; this document records the verified value 3.
- Content height: `max(activeNode.y + activeNode.height) * sy` (`7D8BB0`–`7D8C09`). This is the content rectangle, not a fixed number of subtitle lines.
- Each active subtitle block is horizontally centered individually: `left + contentWidth/2 - node.width*sx/2` (`7D9016`–`7D903F`). Internal multi-line alignment remains controlled by the text engine and was not established here.
- Background fill uses `(R,G,B,A)=(0,0,0,0.5)` from `1817F42E0` (`7D8CBA`, passed to primitive calls).
- Background has reference corner radius 16 pixels: `32.0 * 0.5 * sy` (`7D8CC5`–`7D8CD2`; `181766150=32`, `181C36990=0.5`). Multiple rectangles and four arc triangle fans construct the rounded shape; arc helper `7D9510` samples 15 boundary points per quarter circle.
- Renderer sets a white text multiplier `(1,1,1,1)` at `7D907A`–`7D9090`. Exact glyph face, glyph outline, and font metrics are not implied by this multiplier.

## Movement, overflow and lifetime

- Update function: `0x1807D92B0` through `0x1807D94F4`. It drains pending items into active items, then traverses every active item.
- Node vertical position is eased toward cumulative preceding heights: `node.y += (targetY-node.y)*0.1`; then `targetY += node.height` (`7D948B`–`7D94A7`; constant `181765A0C=0.1`). This is per update, so do not describe it as a verified frame-rate-independent time constant.
- Queue creation `7D99E0` measures actual text, then enqueues it. No fixed subtitle-count limit was found within these creation/update/render functions. This is a bounded negative finding, not a proof that no other producer applies a limit.
- Optional single-item overflow mode clips its visible height to 128 reference pixels if measured height exceeds 128 (`7D9BB9`–`7D9C12`; `181765F60=128`). This mode depends on the request flag; it is not applied unconditionally.
- Overflow scroll start is `(UTF16Length / measuredFullHeight)*13`; end is `start+(measuredFullHeight-128)/10` (`7D9BDF`–`7D9C05`; constants `1817F42D0=13`, `181765A44=10`). Render computes `t=clamp((elapsed-start)/(end-start),0,1)` and subtracts `(fullHeight-128)*t*sy` from Y (`7D9129`–`7D917D`).
- Callback at node `+0x80`, vtable slot `+0x10`, can preserve the hold counter. Once absent, hold `+0x3C` decreases by update delta. Once hold is negative, the head-of-list expiration policy permits decrementing `+0x44`; node removal requires both expiration conditions (`7D9393`–`7D948B`).
- **No linear text-alpha fade was established.** Renderer does not read node `+0x44` or `+0x3C` and does not use them to multiply text/background alpha. `+0x44` is therefore an expiration/removal grace counter based on present evidence, not a proved fade-alpha duration. There may be deeper rendering behavior, but it is not demonstrated here.

## Style and remaining limits

- Manager constructor `7D7C20` initializes style at `manager+0x80` through `1137C20`, sets style `+0x0C/+0x10=2.0` using `113A3E0`, and sets style `+0x04/+0x08=1.0` using `113A3F0`. Render replaces the latter with `sx/sy`.
- `1137C20` initializes the style's raw words/floats to `1,1,1,1,1,0,0,1,0,0,1,10,0,1` across its 0x38 bytes. Semantic names for every field are not established. The two `2.0` fields are routed into the font resource API (`11388E0` → `11744D0`), but this alone does not prove whether they are pixel font size, border parameters, or another metric. Do not label them as exact 2-pixel outlines without corroboration.
- Font resource comes from `manager+0x78`; `11517A0` produces the rendering context and `1171670` copies style/text into it. The exact face, reference pixel size, internal line spacing, and native rasterization remain unknown from this pass.
- Using an external TTF and ImGui therefore remains an approximation of the native font metrics, even with the verified placement, width, rounded background, lifetime, and movement rules implemented.
