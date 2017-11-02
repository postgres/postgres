LOAD 'passwordcheck';

CREATE USER regress_user1;

-- ok
ALTER USER regress_user1 PASSWORD 'a_nice_long_password';

-- error: too short
ALTER USER regress_user1 PASSWORD 'tooshrt';

-- error: contains user name
ALTER USER regress_user1 PASSWORD 'xyzregress_user1';

-- error: contains only letters
ALTER USER regress_user1 PASSWORD 'alessnicelongpassword';

-- encrypted ok (password is "secret")
ALTER USER regress_user1 PASSWORD 'md51a44d829a20a23eac686d9f0d258af13';

-- error: password is user name
ALTER USER regress_user1 PASSWORD 'md5e589150ae7d28f93333afae92b36ef48';

DROP USER regress_user1;
