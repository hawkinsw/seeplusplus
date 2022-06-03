package main

//
// #include <stdint.h>
// typedef struct {
//   uint32_t annotation_points;
//   char **annotations;
//   char *formatted;
// } FormatAnnotateResult;
import "C"
import "go/format"

type Annotation struct {
	index      uint32
	annotation string
}

//export FormatAnnotate
func FormatAnnotate(toformat *C.char) *C.FormatAnnotateResult {
	result := (*C.FormatAnnotateResult)(C.malloc(C.sizeof_FormatAnnotateResult))
	result.annotation_points = C.uint32_t(5)
	return result
}

func doFormatAnnotate(toFormat *string) (*string, *[]Annotation, error) {
	formatted, err := format.Source([]byte(*toFormat))
	if err != nil {
		return nil, nil, err
	}
	formatted_str := string(formatted)
	return &formatted_str, nil, nil
}

func main() {}
