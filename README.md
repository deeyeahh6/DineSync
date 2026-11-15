© 2026 deeyeahh6. All rights reserved.
# DineSync – One Table. One Shared Experience.
QR-based table ordering with a shared cart. HTML/CSS/JS frontend, C backend.

## Environment variables (set these on Render)
- QR_SECRET   – long random text. Keeps every table's QR code permanent. Never change it unless you want new QR codes.
- STAFF_PIN   – PIN for the Staff dashboard  (/#/staff)
- KITCHEN_PIN – PIN for the Kitchen dashboard (/#/kitchen)

Local run: `gcc -O2 -o dinesync server.c && QR_SECRET=abc STAFF_PIN=1234 KITCHEN_PIN=5678 ./dinesync 8080`
