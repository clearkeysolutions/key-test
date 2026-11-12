# ML locator comparison (4000 cases, identical frames)

| | Classical | ML proposals first | ML + gate |
|---|---|---|---|
| Valid symbols decoded | 58.8% (2116/3600) | 61.1% (2198/3600) | 60.6% (2183/3600) |
| Readable from true corners, decoded | 86.3% (2071/2399) | 89.5% (2147/2399) | 88.9% (2133/2399) |
| Wrong payloads | 0 | 0 | 0 |
| Time per frame, median / p95 (ms) | 79 / 630 | 129 / 871 | 128 / 849 |
| Frames that decoded, median (ms) | 61 | 95 | 93 |
| Frames that did not decode, median / p95 (ms) | 181 / 820 | 311 / 1132 | 307 / 1130 |
| Dense (v25-31), readable, decoded | 371/461 | 394/461 | 394/461 |

Gained vs classical: ML 82, ML + gate 82. Lost vs classical: ML 0, ML + gate 15.

| Stratum (readable from true corners) | Classical | ML | ML + gate |
|---|---|---|---|
| S01 | 179/189 | 182/189 | 182/189 |
| S02 | 160/179 | 164/179 | 164/179 |
| S03 | 112/128 | 113/128 | 113/128 |
| S04 | 125/142 | 129/142 | 125/142 |
| S05 | 153/169 | 155/169 | 155/169 |
| S06 | 70/92 | 74/92 | 74/92 |
| S07 | 65/90 | 70/90 | 70/90 |
| S08 | 141/161 | 147/161 | 147/161 |
| S09 | 91/109 | 97/109 | 97/109 |
| S11 | 44/56 | 47/56 | 47/56 |
| S12 | 85/106 | 96/106 | 96/106 |
| S13 | 111/118 | 112/118 | 104/118 |
| S14 | 155/176 | 162/176 | 162/176 |
| S15 | 144/165 | 148/165 | 148/165 |
| S16 | 139/167 | 144/167 | 144/167 |
| S17 | 129/158 | 138/158 | 138/158 |
| S18 | 111/127 | 111/127 | 109/127 |
| S19 | 57/67 | 58/67 | 58/67 |
