exec sql include sqlca;

exec sql whenever not found do break;
exec sql whenever sqlerror sqlprint;
