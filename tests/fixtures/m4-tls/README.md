# Local Android TLS fixture

`server-key.pem` is a public, synthetic test private key. Never use it outside the loopback fixture.
The self-signed certificate is trusted only for localhost/127.0.0.1 by the sample Debug network configuration.
Neither key nor certificate enters SDK or sample Release artifacts. Expiration requires regenerating this fixture
and its Debug certificate copy together; the runner verifies the copies agree.
