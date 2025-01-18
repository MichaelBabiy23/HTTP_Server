echo
echo "----- MALFORMED REQUEST TESTS (via netcat) -----"

# (1) Missing 'HTTP/1.0' part
#    We expect the server to see "GET /hello\r\n\r\n" with no version and respond with 400 or similar.
(
  echo -e "GET /hello\r\n\r\n"
) | nc -w 2 "$HOST" "$PORT" > /tmp/malformed_out1 2>/dev/null

first_line=$(head -n1 /tmp/malformed_out1)
echo "Missing HTTP version => $first_line (likely 400?)"


# (2) Weird method => XYZZY
#    We expect 501 since the server only supports GET.
(
  echo -e "XYZZY / HTTP/1.0\r\n\r\n"
) | nc -w 2 "$HOST" "$PORT" > /tmp/malformed_out2 2>/dev/null

first_line=$(head -n1 /tmp/malformed_out2)
echo "Weird method => $first_line (likely 501?)"


# (3) Partial request line (no \r\n)
#    We send "GET /incomplete HTTP/1.0" but never a newline or CRLF.
#    'nc -w 2' will close the socket after 2s of inactivity,
#    causing the server to see EOF and (hopefully) return 400.
echo -n "GET /incomplete HTTP/1.0" | nc -w 2 "$HOST" "$PORT" > /tmp/malformed_out3 2>/dev/null

first_line=$(head -n1 /tmp/malformed_out3)
echo "Partial request => $first_line (likely 400?)"
