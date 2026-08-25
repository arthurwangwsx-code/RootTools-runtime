#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime_observer.h"

int main(void) {
    char path[]="/tmp/roottools-dpkg-status-XXXXXX";
    int fd=mkstemp(path);assert(fd>=0);
    const char *status=
        "Package: re.frida.server\nName: Frida Server\nStatus: install ok installed\nVersion: 17.2.5\n\n"
        "Package: ellekit\nName: ElleKit\nStatus: install ok installed\nVersion: 1.1.2\n\n";
    assert(write(fd,status,strlen(status))==(ssize_t)strlen(status));close(fd);
    setenv("ROOTTOOLS_DPKG_STATUS",path,1);
    char *frida=rt_frida_status_json();
    assert(frida&&strstr(frida,"re.frida.server")&&strstr(frida,"17.2.5"));
    assert(strstr(frida,"\"scriptExecutionExposed\":false"));
    assert(strstr(frida,"\"arbitraryAttachExposed\":false"));
    free(frida);
    char *ellekit=rt_ellekit_status_json();
    assert(ellekit&&strstr(ellekit,"\"id\":\"ellekit\"")&&strstr(ellekit,"1.1.2"));
    assert(strstr(ellekit,"\"rawHookAPIExposed\":false"));
    assert(strstr(ellekit,"\"arbitraryInjectionExposed\":false"));
    free(ellekit);unlink(path);
    puts("runtime_observer_test: PASS");
    return 0;
}
