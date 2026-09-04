import numpy as np
import matplotlib.pyplot as plt

def trigger_FLT0(channel, dict_trigger_parameter):
    # Find the indices where the signal crosses the first threshold (T1)
    index_t1_crossing = np.where(channel > dict_trigger_parameter["th1"])[0]
    
    # Lists to store results
    T1_indices = []   # Indices of T1 crossings
    T1_amplitudes = []  # Amplitudes at T1 crossings
    NC_values = []  # Number of T2 crossings for each T1 (the T1 is included in NC)

    # if len(index_t1_crossing)>0:
    #     plt.figure()
    #     plt.plot(channel)
    #     plt.plot([0,len(channel)],[dict_trigger_parameter["th1"],dict_trigger_parameter["th1"]])
    #     plt.plot([0,len(channel)],[dict_trigger_parameter["th2"],dict_trigger_parameter["th2"]])
    #     plt.plot(index_t1_crossing,channel[index_t1_crossing],'or')

    # Process each T1 crossing
    for index_T1 in index_t1_crossing:
        #print("** Now checking T1 @ index =" ,index_T1)
        # Check if the T1 index is greater than 100 (bug linked to the notch filter: artificial peak that appears)
        ## to be corrected
        if index_T1 <= 100:
            continue  # Skip this T1 if its index is not greater than 100
        
        start = max(0, index_T1 - dict_trigger_parameter['t_quiet'] // 2) # 2 (ns) / counts
        end = index_T1
        
        # Check if the signal before T1 is below the first threshold (T1 is valid)
        if np.all(channel[start:end] <= dict_trigger_parameter["th1"]):
            
            # Extract the period after T1 to find T2 crossings
            period_after_T1 = channel[index_T1: index_T1 + dict_trigger_parameter['t_period'] // 2]
            positive_T2_crossing = (period_after_T1 > dict_trigger_parameter['th2']).astype(int)
            
            # Search for positive T2 crossings
            mask_T2_crossing_positive = np.diff(positive_T2_crossing) == 1
            index_T2 = np.where(mask_T2_crossing_positive)[0] + 1 + index_T1
            index_T2 = np.insert(index_T2, 0, index_T1) #add T1 in index crossings in the first component
            n_T2_crossing = len(index_T2)   # Number of T2 crossings (including the T1 crossing itself)
            valid_T1 = True

            # Check for maximum separation condition between T2 crossings
            for i, j in zip(index_T2[:-1], index_T2[1:]):
                time_separation = (j - i) * 2  # Calculate the time separation between consecutive T2 crossings. 2 (ns) / counts
                
                # If the separation is too large, mark T1 as invalid
                if time_separation > dict_trigger_parameter["t_sepmax"]:
                    valid_T1 = False
                    #print("Killed by Tsepmax <",time_separation)
                    #if len(index_T2)>1:
                    #    plt.figure()
                    #    plt.plot(channel)
                    #    plt.plot([0,len(channel)],[dict_trigger_parameter["th1"],dict_trigger_parameter["th1"]],'-r')
                    #    plt.plot([0,len(channel)],[dict_trigger_parameter["th2"],dict_trigger_parameter["th2"]],'-g')
                    #    plt.plot(index_t1_crossing,channel[index_t1_crossing],'or')
                    #    plt.plot(index_T2,channel[index_T2],'xg')
                    #    plt.show()

                    break

            # If the number of T2 crossings is out of bounds, ignore this T1
            if n_T2_crossing < dict_trigger_parameter["nc_min"] or n_T2_crossing > dict_trigger_parameter["nc_max"]:
                valid_T1 = False

            if valid_T1:
                # If T1 is valid, record the number of T2 crossings
                NC_values.append(n_T2_crossing)
                T1_indices.append(index_T1)
                T1_amplitudes.append(channel[index_T1])

            #if (valid_T1):
            #    plt.figure()
            #    plt.plot(channel)
            #    plt.plot([0,len(channel)],[dict_trigger_parameter["th1"],dict_trigger_parameter["th1"]],'-r')
            #    plt.plot([0,len(channel)],[dict_trigger_parameter["th2"],dict_trigger_parameter["th2"]],'-g')
            #    plt.plot(index_t1_crossing,channel[index_t1_crossing],'or')
            #    plt.plot(index_T2,channel[index_T2],'xg')

            #    fft_0 = np.abs( np.fft.rfft(channel) )
            #    fft_freq = np.fft.rfftfreq( len(channel) )*500 # [MHz]
            #    plt.figure()
            #    plt.plot(fft_freq,fft_0)

            #    plt.show()

      
    if T1_indices:
        return T1_indices, T1_amplitudes, NC_values
    else:
        return [], [], [] # Do nothing if no valid T1 crossings were found
