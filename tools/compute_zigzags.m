#!/usr/bin/env octave

% make sure tools are in the path
pin = program_invocation_name;
addpath(pin(1:(length(pin)-length(program_name))));

args = argv();
bsize = str2num(args{1});
output = args{2};
files = args(3:length(args));
coeffs = zeros(1, bsize^2);
total = 0;

for i=1:length(files)
  t = importdata(files{i}, " ");
  % select relevant rows
  rows = find(t(:,1) >= bsize);
  % pick out relevant coeffs
  t = t(rows,2:1+bsize^2);
  total = total + size(t, 1);
  coeffs = coeffs .+ sum(t .^ 2);
end

% gen_zigzag expects a column vector, so this must be transposed
v = coeffs' / total;

% generate zigzag
if bsize == 4
  gen_zigzag4(v, output);
elseif bsize == 8
  gen_zigzag8(v, output);
elseif bsize == 16
  gen_zigzag16(v, output);
elseif bsize == 32
  gen_zigzag32(v, output);
else
  printf("error: invalid block size\n");
end
