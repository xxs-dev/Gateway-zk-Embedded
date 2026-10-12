# Lanhu Bitmap Provenance

Source: the user's EMS 2.0 Lanhu project `be19aa1e-6480-4ca0-95a1-5ec773b5bd3e`, observed through the user's open browser tab on 2026-10-12.

Extraction is reproducible with `products/ems/mobile/2.0/tools/import-lanhu-assets.py`. Input boards are normalized to 1440 x 810. Some original exports contain one additional pixel row.

| Board | Export filename | Uses |
| --- | --- | --- |
| Meter | da5f9ac1e70cbd2c.png | heading, summary icons, navigation glyphs |
| Topology | 9e8a504aa791edc7.png | logo, meter, storage vehicle, diesel engine |
| Self-check | 23599ac4a4874ce8.png | guide icons, normal/abnormal glyphs |
| Deployment | d261875f09c3b072.png | voltage, phase and diesel icons |
| Alarm | 2c336ab5c85e8b74.png | counter icons |
| Strategy | 53fc3fd279c38209.png | seven strategy icons |
| PCS | a087129870c2b684.png | mode, phase and command icons |
| TMS | 5fe0b5dd94c61efc.png | temperature icon |
| MCCB | 6bb78a777ff0dd72.png | breaker icon |
| Diesel | 3d079696fe3b4c22.png | diesel command icons |
| Cell trends | 3e90ca3567ae7501.png | cell voltage and temperature headings |
| Custom trends | 12c8b3231468cbd9.png | power heading |

Navigation tiles are reconstructed from cropped glyphs with the approved colors and dimensions. The idle vehicle is blended with the design background. No full-board image is used as an application screen; values and actions remain native runtime elements. No source browser credentials or tracking URLs are stored here.
