# SMS HD Pack System - Path Separator Fix

## ?? **Issue Identified: Mixed Path Separators**

### **Root Cause:**
The file creation was failing due to **mixed path separators** in the paths. The logs showed:

```
? BROKEN PATH:
C:\Users\dambi\Documents\Mesen2/HdPacks/Sonic The Hedgehog 2 _Europe_ Brazil_ _En_/hires.txt
```

Notice the mix of:
- `\` (backslashes) - correct for Windows
- `/` (forward slashes) - incorrect for Windows in this context

### **Fixes Applied:**

1. **Path Normalization** - Added `NormalizePath()` function that:
   - Converts all path separators to the correct platform-specific ones
   - On Windows: `/` ? `\`
   - On Linux/Mac: `\` ? `/`

2. **Enhanced ROM Name Sanitization** - Improved `SanitizeRomName()`:
   - Removes consecutive underscores (`__` ? `_`)
   - Shorter length limit (60 chars instead of 80)
   - Better cleanup of trailing characters

3. **Consistent Path Handling** - Added normalization at multiple points:
   - In `StartHdPackDumping()` when creating save folder
   - In `SaveHdPack()` before file operations
   - In both directory creation and file creation

4. **Test Function** - Added `TestFileCreation()` for debugging

### **Expected Results:**

**Before Fix:**
```
? C:\Users\dambi\Documents\Mesen2/HdPacks/Sonic The Hedgehog 2 _Europe_ Brazil_ _En_/hires.txt
? Failed to create manifest file
```

**After Fix:**
```
? C:\Users\dambi\Documents\Mesen2\HdPacks\Sonic_The_Hedgehog_2_Europe_Brazil_En\hires.txt
? Manifest file opened successfully
```

## ?? **Testing Instructions:**

### **Method 1: Load SMS Game (Recommended)**
1. **Load any SMS game** in Mesen2
2. **Check Log Window** for new messages:
   ```
   [SMS HD Pack] Normalized save folder: C:\Users\[user]\Documents\Mesen2\HdPacks\[clean_name]
   [SMS HD Pack] Normalized manifest path: C:\Users\[user]\Documents\Mesen2\HdPacks\[clean_name]\hires.txt
   [SMS HD Pack] ? Manifest file opened successfully
   ```
3. **Close the game** to trigger save
4. **Check the folder** - should now contain `hires.txt`

### **Method 2: Use Test Function (If Available)**
If you can access the console/debugger:
```cpp
SmsHdPackApi::TestFileCreation(emulator);
```

This will create a test file to verify the file system works.

## ?? **Expected File Locations:**

**Cleaned ROM Names:**
- `"Sonic The Hedgehog 2 (Europe, Brazil) (En)"` ? `"Sonic_The_Hedgehog_2_Europe_Brazil_En"`
- `"Castle of Illusion Starring Mickey Mouse (USA, Europe, Brazil) (En) (Rev 1)"` ? `"Castle_of_Illusion_Starring_Mickey_Mouse_USA_Europe_Brazil_En_Rev_1"`

**Final Paths:**
```
C:\Users\[USERNAME]\Documents\Mesen2\HdPacks\Sonic_The_Hedgehog_2_Europe_Brazil_En\hires.txt
C:\Users\[USERNAME]\Documents\Mesen2\HdPacks\Castle_of_Illusion_Starring_Mickey_Mouse_USA_Europe_Brazil_En_Rev_1\hires.txt
```

## ?? **Key Changes Made:**

1. **NormalizePath()** - Fixes `/` vs `\` issues
2. **Better sanitization** - Cleaner ROM folder names  
3. **Consistent normalization** - Applied everywhere paths are used
4. **Enhanced debugging** - More detailed logging
5. **Test function** - Independent verification

## ? **Status: READY FOR TESTING**

The path separator issue should now be completely resolved. The system will now create proper Windows-compatible paths and the `hires.txt` file should be created successfully!

Try loading an SMS game again and check if you see the new normalized paths in the logs and if the file gets created properly.