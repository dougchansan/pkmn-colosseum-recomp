/* PokemonColosseumRecomp.app's executable: starts the launcher that sits
 * next to the app (the package keeps its flat layout: Sys, Mods, User and
 * the game are found next to the launcher). The launcher and the game it
 * starts run as this app's children, so macOS attributes them to the app:
 * it asks once for Local Network access (Info.plist explains why) and lists
 * the app under Privacy & Security > Local Network.
 *
 * A downloaded app opened straight from the zip's folder can run from a
 * read-only copy elsewhere (App Translocation); the original location is
 * looked up then, so the launcher next to it is still found. */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

/* The bundle's original path when macOS runs a translocated copy. */
static int original_bundle_path(const char *bundle, char *out, size_t size) {
  void *security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
  if (!security)
    return 0;
  typedef Boolean (*IsTranslocated)(CFURLRef, bool *, CFErrorRef *);
  typedef CFURLRef (*OriginalPath)(CFURLRef, CFErrorRef *);
  IsTranslocated is_translocated = (IsTranslocated)dlsym(security, "SecTranslocateIsTranslocatedURL");
  OriginalPath original = (OriginalPath)dlsym(security, "SecTranslocateCreateOriginalPathForURL");
  int ok = 0;
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)bundle,
                                                         (CFIndex)strlen(bundle), true);
  bool translocated = false;
  if (url && is_translocated && original && is_translocated(url, &translocated, NULL) &&
      translocated) {
    CFURLRef real = original(url, NULL);
    if (real) {
      ok = CFURLGetFileSystemRepresentation(real, true, (UInt8 *)out, (CFIndex)size);
      CFRelease(real);
    }
  }
  if (url)
    CFRelease(url);
  return ok;
}

int main(int argc, char **argv) {
  (void)argc;
  char self[PATH_MAX];
  uint32_t self_size = sizeof(self);
  char resolved[PATH_MAX];
  if (_NSGetExecutablePath(self, &self_size) != 0 || !realpath(self, resolved))
    return 1;
  /* .../PokemonColosseumRecomp.app/Contents/MacOS/launch -> the .app */
  char bundle[PATH_MAX];
  snprintf(bundle, sizeof(bundle), "%s", resolved);
  for (int i = 0; i < 3; ++i) {
    char *slash = strrchr(bundle, '/');
    if (!slash)
      return 1;
    *slash = 0;
  }
  char original[PATH_MAX];
  if (original_bundle_path(bundle, original, sizeof(original)))
    snprintf(bundle, sizeof(bundle), "%s", original);
  char *slash = strrchr(bundle, '/');
  if (!slash)
    return 1;
  *slash = 0; /* the package folder */
  char launcher[PATH_MAX];
  snprintf(launcher, sizeof(launcher), "%s/PokemonColosseumRecomp", bundle);

  /* Finder's -psn_ argument on older systems is not for the launcher. */
  char *args[64];
  int n = 0;
  args[n++] = launcher;
  for (int i = 1; argv[i] && n < 63; ++i)
    if (strncmp(argv[i], "-psn_", 5) != 0)
      args[n++] = argv[i];
  args[n] = NULL;

  pid_t pid;
  if (posix_spawn(&pid, launcher, NULL, NULL, args, environ) != 0) {
    fprintf(stderr, "cannot start %s\n", launcher);
    return 1;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
    ;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
