exec sql include sqlca;

exec sql whenever not found do set_not_found();
exec sql whenever sqlerror sqlprint;
