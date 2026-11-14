*** Settings ***
Documentation     End-to-end tests against a disposable docker compose stack (project ``pgdemo-e2e``).
...
...               Each child suite has its own Suite Setup and Suite Teardown. Setup builds a
...               fresh stack for that suite alone; teardown runs ``down -v``. Suites do not
...               share devices, users or tokens. This final teardown removes anything a
...               failed suite left behind.
...
...               Needs Docker with compose v2, curl and jq on the host. The development stack
...               must be down: both publish 127.0.0.1:8080 and use the 172.28.* subnets.
...               The console shows Robot's own progress. ``--variable E2E_DEBUG:true``
...               also echoes every command and response as it happens.
Resource          resources/stack.resource
Suite Teardown    Destroy Stack
