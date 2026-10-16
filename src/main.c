#include "cli.h"
#include "mc/platform/platform.h"

int main(int argc, char **argv)
{
    // Before any thread starts, so that every thread leaves these signals to
    // the one that waits for them.
    signals_block();
    return cli_main(argc, argv);
}
