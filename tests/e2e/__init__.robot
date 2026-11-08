*** Settings ***
Documentation     End-to-end tests against a disposable docker compose stack (project ``pgdemo-e2e``).
...
...               Suite setup: fresh volumes, random passwords, and the server started without
...               the controller. Child suites run in file-name order and share that state.
...               Suite teardown: compose logs into the output directory, then ``down -v``.
...
...               Needs Docker with compose v2, curl and jq on the host. The development stack
...               must be down: both publish 127.0.0.1:8080 and use the 172.28.* subnets.
...               The console shows Robot's own progress. ``--variable E2E_DEBUG:true``
...               also echoes every command and response as it happens.
Resource          resources/stack.resource
Suite Setup       Start Stack
Suite Teardown    Destroy Stack
