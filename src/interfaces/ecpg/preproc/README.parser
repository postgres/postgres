ECPG modifies and extends the core grammar in a way that
1) every token in ECPG is <str> type. New tokens are
   defined in ecpg.tokens, types are defined in ecpg.type
2) most tokens from the core grammar are simply converted
   to literals concatenated together to form the SQL string
   passed to the server, this is done by parse.pl.
3) some rules need side-effects, actions are either added
   or completely overridden (compared to the basic token
   concatenation) for them, these are defined in ecpg.addons,
   the rules for ecpg.addons are explained below.
4) new grammar rules are needed for ECPG metacommands.
   These are in ecpg.trailer.
5) ecpg.header contains common functions, etc. used by
   actions for grammar rules.

In "ecpg.addons", every modified rule follows this pattern:
       ECPG: dumpedtokens postfix
where "dumpedtokens" is simply tokens from core gram.y's
rules concatenated together. e.g. if gram.y has this:
       ruleA: tokenA tokenB tokenC {...}
then "dumpedtokens" is "ruleAtokenAtokenBtokenC".
"postfix" above can be:
a) "block" - the automatic rule created by parse.pl is completely
    overridden, the code block has to be written completely as
    it were in a plain bison grammar
b) "rule" - the automatic rule is extended on, so new syntaxes
    are accepted for "ruleA". E.g.:
      ECPG: ruleAtokenAtokenBtokenC rule
          | tokenD tokenE { action_code; }
          ...
    It will be substituted with:
      ruleA: <original syntax forms and actions up to and including
                    "tokenA tokenB tokenC">
             | tokenD tokenE { action_code; }
             ...
c) "addon" - the automatic action for the rule (SQL syntax constructed
    from the tokens concatenated together) is prepended with a new
    action code part. This code part is written as is's already inside
    the { ... }

Multiple "addon" or "block" lines may appear together with the
new code block if the code block is common for those rules.
