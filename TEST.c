#include "types.h"
#include "user.h"
int main() {
schedlog(10000);
for (int i = 0; i < 3; i++) {
if (priofork(0) == 0) {
char *argv[] = {"TEST_loop", 0};
exec("TEST_loop", argv);
}
}
for (int i = 0; i < 3; i++) {
wait();
}
printf(1, "SHUTTING DOWN \n");
shutdown();
}