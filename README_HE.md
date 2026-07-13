<div dir="rtl">

# GiantLLM

**מודלים מקומיים ענקיים על חומרה צנועה.**

GiantLLM הוא fork עצמאי של `llama.cpp` שמיועד להרצת מודלי שפה גדולים, ובמיוחד מודלי MoE, גם כאשר אין מספיק RAM כדי להחזיק את כל משקלי המומחים בזיכרון בו-זמנית.

במצב `explicit`, משקלי המומחים המנותבים נשארים בקובץ GGUF. רק המומחים שה-router בחר נטענים למטמוני RAM מוגבלים לפי שכבה. יתר משקלי המודל, שרת ההסקה, ממשק הצ'אט, GGML ותמיכת הארכיטקטורות מגיעים מ-llama.cpp.

זוהי גרסת **v0.2.0-alpha**. Qwen3.5/Qwen3.6 MoE נבדקו בבדיקות דטרמיניסטיות ובבדיקת streaming קצרה עם מודל אמיתי. Gemma 4 26B-A4B ומודלי MoE נוספים בעלי פריסת expert tensors תואמת מתקבלים באמצעות בדיקת layout כללית. עדיין אין הבטחה שכל quantization או מודל יתאים למחשב מסוים.

## התקנה קבועה ב-Windows

1. הורידו מדף ה-Release את `giantllm-0.2.0-alpha-windows-x64-avx2-setup.exe`.
2. הפעילו את המתקין.
3. המתקין מתקין לתיקיית המשתמש, מוסיף אפשרות הסרה ב-Windows ויוצר קיצור דרך בתפריט Start. אפשר לבחור גם קיצור דרך לשולחן העבודה והוספה ל-PATH.
4. הפעילו `GiantLLM` מתפריט Start.

## הפעלה ללא התקנה

1. חלצו את חבילת ה-ZIP לתיקייה עם הרשאת כתיבה.
2. הפעילו `GiantLLM.exe`.
3. בחרו קובץ `.gguf` מקומי.
4. החילו את המלצת החומרה השמרנית.
5. הגדירו אורך הקשר, MTP ו-MoE paging לפי הצורך.
6. הפעילו את השרת ופתחו את ממשק הצ'אט.

## מה ניתן לשלוט מהממשק?

- נתיב המודל ומצב טעינת המומחים.
- גודל מטמון המומחים, מספר slots ומספר תהליכי קריאה מה-SSD.
- אורך ההקשר, batch, microbatch ומספר שיחות מקבילות.
- סוגי KV cache, שמירת הקשר ומטמון prompts.
- הפעלה והגדרה של MTP במודלים תואמים.
- סטטוס השרת, PID, לוגים, שימוש בזיכרון ומדדי paging חיים.
- הורדת קובץ אבחון ואיפוס סטטיסטיקות.

אותן הגדרות שרת זמינות גם בתוך ממשק הצ'אט תחת **Settings -> Server**. שינוי הגדרות שדורשות טעינת מודל מחדש מבצע restart מבוקר של השרת.

## מצבי טעינה

- `off`: טעינה רגילה של llama.cpp ללא מנגנון GiantLLM.
- `os`: שימוש רגיל ב-`mmap` ובמנהל הזיכרון של מערכת ההפעלה.
- `explicit`: טעינת מומחים מדויקת מהכונן למטמוני slots מוגבלים.
- `auto`: בחירה ב-explicit כאשר הוגדר תקציב cache או slots; אחרת שימוש ב-OS paging.

SSD מומלץ מאוד. מנגנון paging אינו מבטל את הזיכרון שנדרש למשקלים קבועים, ל-KV cache, להקשר ולתקורות הריצה.

## בנייה מקוד המקור

```powershell
git clone https://github.com/menachem-dadon/GiantLLM.git
cd GiantLLM
pwsh scripts/build-windows.ps1 -Variant avx2 -RunTests
.\build-windows-avx2\bin\GiantLLM.exe
```

ליצירת ZIP ומתקין:

```powershell
.\scripts\package-windows.ps1 -BuildDir .\build-windows-avx2 -Variant avx2
.\scripts\build-installer.ps1
```

## איפה נמצא הקוד של GiantLLM?

- `src/moe-paging/`: האלגוריתם, המטמון, קריאת הקבצים והסטטיסטיקות.
- `tools/giantllm/`: מנהל התוכנה וה-dashboard.
- `tools/giantllm-iobench/`: כלי בדיקת הכונן.
- `SettingsChatServerTab.svelte`: מסך הגדרות השרת בתוך ממשק הצ'אט.
- `scripts/` ו-`packaging/`: בנייה, חבילות ומתקין.
- `assets/branding/`: אייקון התוכנה וה-dashboard.

למיפוי מלא וברור קראו את [מפת הפרויקט](docs/PROJECT_MAP.md).

## הקשר ל-llama.cpp

GiantLLM כולל snapshot מלא של llama.cpp ולא תלות חיצונית. לכן קיימות בו התכונות שהיו בגרסת upstream הרשומה ב-`UPSTREAM_BASE.txt`, בתוספת יכולות GiantLLM. תכונות חדשות שנוספו מאוחר יותר ל-llama.cpp אינן מגיעות אוטומטית ודורשות merge מבוקר ובדיקות חוזרות.

## פרטיות

אין telemetry, analytics, העלאת קריסות או הורדת מודלים אוטומטית. מנהל GiantLLM מאזין רק ב-`127.0.0.1`. חשיפת `llama-server` לרשת דורשת API key והגדרות אבטחה מתאימות.

## רישוי

קוד upstream של llama.cpp והשינויים שבתוך קבצי upstream נשארים תחת MIT. הרכיבים המקוריים והעצמאיים של GiantLLM נמצאים תחת PolyForm Noncommercial 1.0.0. הגבול המדויק מפורט ב-[LICENSING.md](LICENSING.md).

מודלים אינם כלולים. כל מודל כפוף לרישיון של מאגר המודל שממנו הורד.

GiantLLM הוא פרויקט קהילתי עצמאי ואינו משויך או מאושר על ידי ggml-org, Qwen, Alibaba Cloud, Google, Gemma או מחברי Colibri.

</div>
