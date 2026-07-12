<div dir="rtl">

# QwenStream

QwenStream הוא fork עצמאי של `llama.cpp` המיועד להרצת מודלי MoE במעבד. במצב `explicit` משקלי המומחים המנותבים נשארים בקובץ GGUF, ורק המומחים שה־router בחר נטענים למטמוני RAM קטנים ומוגבלים לכל שכבה.

זוהי גרסת **v0.1.0-alpha**. הליבה, קורא הקבצים האסינכרוני של Windows, מטמון LRU, שילוב CLI/שרת, launcher ובדיקות דטרמיניסטיות נבנו ונבדקו. בדיקת streaming קצרה עברה על Qwen3.6 35B-A3B IQ4 GGUF אמיתי לאחר הקטנה אוטומטית של ה־microbatch כך שיתאים למטמון ה־slots. מטריצת האימות המלאה של 128 tokens מול מסלול resident עדיין לא בוצעה, ולכן אין עדיין הבטחה להתאמה ל־16GB או לביצועים מסוימים.

## התחלה מהירה

1. חלצו את חבילת ה־ZIP לתיקייה עם הרשאת כתיבה.
2. הפעילו `QwenStream.exe`.
3. הזינו נתיב לקובץ `.gguf` מקומי.
4. השאירו את השרת על `127.0.0.1`, בחרו תקציב מטמון והפעילו.
5. פתחו את ממשק השיחה מתוך לוח הבקרה.

הרצה ישירה:

```powershell
llama-cli.exe -m C:\models\qwen.gguf --moe-paging explicit --moe-cache-mib 1536 --moe-io-threads 3 -p "שלום"
```

שרת:

```powershell
llama-server.exe -m C:\models\qwen.gguf --host 127.0.0.1 --port 8080 --parallel 1 --moe-paging explicit --moe-cache-mib 1536
```

## מצבי טעינה

- `off` — המסלול הרגיל של llama.cpp.
- `os` — שימוש רגיל ב־`mmap` ובניהול הדפים של מערכת ההפעלה.
- `explicit` — טעינת מומחים מדויקת מהכונן למטמוני slots.
- `auto` — explicit כאשר הוגדר תקציב/מספר slots, אחרת OS paging. ה־launcher מציע ערך שמרני לפי החומרה.

SSD מומלץ מאוד. דיסק קשיח מכני עלול להיות איטי מדי. המטמון אינו מחליף את הזיכרון הדרוש למשקלים הקבועים, ל־KV cache ולתקורות הריצה.

אין טלמטריה, אנליטיקה או העלאת קריסות. אין הורדה אוטומטית של מודלים. קראו את [מדריך המשתמש](docs/USER_GUIDE.md), [פתרון התקלות](docs/TROUBLESHOOTING.md) ו־[הנחיות הרישוי למודלים](docs/MODEL_LICENSES.md).

QwenStream הוא פרויקט קהילתי עצמאי ואינו משויך או מאושר על ידי Qwen, Alibaba Cloud, ggml-org או מחברי Colibri.

</div>
