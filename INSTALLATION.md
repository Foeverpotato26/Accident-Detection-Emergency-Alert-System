# Installation and publication guide

Full instructions are in the main [README](README.md). Quick checklist:

## 1. Configure
1. `ESP32/AccidentAlert/config.example.h` → copy to `config.h`, fill Wi-Fi and the Firebase URL.
2. Edit `docs/config.js` (URL only).
3. Firebase: create a Realtime Database and publish the rules from the README.

## 2. Flash and test
Upload the sketch (ESP32 Dev Module, 115200 baud), check the Serial Monitor for `[FIREBASE] PUT /accidentAlert/current -> HTTP 200`.

## 3. Publish on GitHub
```bash
cd Accident-Alert-System
git init
git add .
git status            # verify that config.h is NOT listed
git commit -m "Initial commit: ESP32 accident alert system"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/Accident-Alert-System.git
git push -u origin main
```
Then enable GitHub Pages (Settings → Pages).

## Troubleshooting
| Symptom | Cause |
|---|---|
| HTTP 401 / 403 | Database rules do not allow access to `accidentAlert` |
| Dashboard: "NO DATA" | ESP32 not uploading yet |
| `Motion sensor NOT DETECTED` | Wiring, or a genuine MPU6050 needing a different library (see README) |
| GPS characters stay at 0 | TX/RX swapped or no power |
