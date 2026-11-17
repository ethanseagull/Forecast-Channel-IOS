// iOS: getting the user's channel files without moving them around.
//
// When the app starts without them, it explains what it needs and opens the
// system folder picker. The user picks the folder where they keep the six
// files (e.g. "On My iPhone > Forecast Channel Files", which survives
// reinstalling the app); they are copied into the app's Documents and
// imported as usual (port/sdk/import.c). Compiled into its own library
// (CMakeLists.txt) so the port's compatibility flags stay away from UIKit.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <SDL3/SDL.h>

#include <port_ios.h>
#include <port_log.h>
#include <port_paths.h>
#include <port_platform.h>

static NSString* const kFiles[] = {@"00000001.app", @"00000006.app", @"00000007.app",
                                   @"00000008.app", @"00000009.app", @"0000000a.app"};

@interface PortPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) BOOL done;
@property(nonatomic, strong) NSURL* url;
@end

@implementation PortPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    self.url = urls.firstObject;
    self.done = YES;
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    self.done = YES;
}
@end

static UIViewController* root_view_controller(void) {
    SDL_PropertiesID props = SDL_GetWindowProperties(port_window());
    UIWindow* window = (__bridge UIWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    UIViewController* vc = window.rootViewController;
    while (vc.presentedViewController != nil) {
        vc = vc.presentedViewController;
    }
    return vc;
}

// Shows the folder picker and waits for the user (the game is not running yet,
// so spinning the run loop here is fine). Returns the folder, or nil.
static NSURL* pick_folder(void) {
    UIViewController* vc = root_view_controller();
    if (vc == nil) {
        port_log("import: no view controller for the folder picker\n");
        return nil;
    }
    PortPickerDelegate* delegate = [[PortPickerDelegate alloc] init];
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
    picker.delegate = delegate;
    picker.allowsMultipleSelection = NO;
    picker.modalPresentationStyle = UIModalPresentationFormSheet;
    [vc presentViewController:picker animated:YES completion:nil];
    while (!delegate.done) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    return delegate.url;
}

// Copies the six files from folder into Documents. Returns how many it found.
static int copy_from(NSURL* folder) {
    BOOL scoped = [folder startAccessingSecurityScopedResource];
    NSFileManager* fm = [NSFileManager defaultManager];
    NSURL* docs = [NSURL fileURLWithPath:[NSString stringWithUTF8String:port_data_dir()] isDirectory:YES];
    // Files may be named with different letter case, or sit in a subfolder
    // (e.g. an unzipped "ForecastChannelFiles" folder inside the one picked).
    NSMutableDictionary<NSString*, NSURL*>* found = [NSMutableDictionary dictionary];
    NSDirectoryEnumerator<NSURL*>* it = [fm enumeratorAtURL:folder
                                 includingPropertiesForKeys:nil
                                                    options:NSDirectoryEnumerationSkipsHiddenFiles
                                               errorHandler:nil];
    for (NSURL* url in it) {
        NSString* name = url.lastPathComponent.lowercaseString;
        for (size_t i = 0; i < sizeof(kFiles) / sizeof(kFiles[0]); i++) {
            if ([name isEqualToString:kFiles[i]] && found[kFiles[i]] == nil) {
                found[kFiles[i]] = url;
            }
        }
    }
    int copied = 0;
    for (NSString* name in found) {
        NSURL* dst = [docs URLByAppendingPathComponent:name];
        [fm removeItemAtURL:dst error:nil];
        NSError* err = nil;
        if ([fm copyItemAtURL:found[name] toURL:dst error:&err]) {
            copied++;
        } else {
            port_log("import: copying %s failed: %s\n", name.UTF8String, err.localizedDescription.UTF8String);
        }
    }
    if (scoped) {
        [folder stopAccessingSecurityScopedResource];
    }
    port_log("import: %d of 6 channel files copied from %s\n", copied, folder.path.UTF8String);
    return copied;
}

bool port_ios_get_channel_files(void) {
    for (int attempt = 0; attempt < 3; attempt++) {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose Folder"},
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
        };
        const SDL_MessageBoxData box = {
            SDL_MESSAGEBOX_INFORMATION,
            port_window(),
            "Forecast Channel files needed",
            attempt == 0 ? "Choose the folder in Files that holds the channel's files: 00000001.app and "
                           "00000006.app to 0000000a.app. Keep that folder outside this app (for example "
                           "On My iPhone > Forecast Channel Files) and you only need to choose it again "
                           "after reinstalling."
                         : "Not all six files were in that folder (00000001.app, 00000006.app to "
                           "0000000a.app). Choose the folder again?",
            2,
            buttons,
            NULL,
        };
        int choice = 0;
        if (!SDL_ShowMessageBox(&box, &choice) || choice != 1) {
            return false;
        }
        NSURL* folder = pick_folder();
        if (folder == nil) {
            continue;
        }
        if (copy_from(folder) == 6) {
            return true;
        }
    }
    return false;
}
