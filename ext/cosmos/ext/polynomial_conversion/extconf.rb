require 'mkmf'

unless $CFLAGS.gsub!(/ -O[\dsz]?/, ' -O3')
  $CFLAGS << ' -O3'
end
if CONFIG['CC'] =~ /gcc/
  $CFLAGS << ' -Wall'
  # Ruby 2.x headers declare method functions as VALUE (*)(), which C23 --
  # GCC 15's default -- reads as taking no arguments, so rb_define_method
  # rejects every one. Build as C17.
  $CFLAGS << ' -std=gnu17'
  if $DEBUG && !$CFLAGS.gsub!(/ -O[\dsz]?/, ' -O0 -ggdb')
    $CFLAGS << ' -O0 -ggdb'
  end
end

create_makefile 'cosmos/ext/polynomial_conversion'
