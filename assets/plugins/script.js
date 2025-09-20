// global variable to keep track of frame number
let frame_num = 0;
let mv_param;

export function setup(args)
{
  // add motion vectors ("mv") to selected features
  args.features.push("mv");


  // initialize variables from input parameter
  try {
    mv_param = MV(args.params);
  } catch (TypeError) {
    mv_param = [10, 10];
  }
  console.log(`Motion vector parameter is ${mv_param}`);
}

export function glitch_frame(frame, stream)
{
  // print stream information from this frame
  console.log(`[${frame_num}] ${JSON.stringify(stream)}`);

  // print available features from this frame
  const features = Object.keys(frame).join(' ');
  console.log(`Available features: ${features}`);

  // override motion vectors
  const mvs = frame.mv?.forward;
  if ( mvs )
  {
    console.log("Overriding motion vectors");
    mvs.fill(mv_param);
  }

  // increment frame_num
  frame_num++;
}
